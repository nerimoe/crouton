/* Copyright 2012 The ChromiumOS Authors
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

/*
 * Basic playback flow:
 *  cras_client_create - Create new structure and set to defaults.
 *  cras_client_connect - Connect client to server - sets up server_fd to
 *    communicate with the audio server.  After the client connects, the server
 *    will send back a message containing the client id.
 *  cras_client_add_stream - Add a playback or capture stream. Creates a
 *    client_stream struct and send a file descriptor to server. That file
 *    descriptor and aud_fd are a pair created from socketpair().
 *  client_connected - The server will send a connected message to indicate that
 *    the client should start receiving audio events from aud_fd. This message
 *    also specifies the shared memory region to use to share audio samples.
 *    This region will be shmat'd.
 *  running - Once the connections are established, the client will listen for
 *    requests on aud_fd and fill the shm region with the requested number of
 *    samples. This happens in the aud_cb specified in the stream parameters.
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE  // For ppoll()
#endif

#include "cras_client.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <sys/eventfd.h>
#include <sys/ipc.h>
#include <sys/mman.h>
#include <sys/param.h>
#include <sys/signal.h>
#include <sys/socket.h>
#include <sys/timerfd.h>
#include <sys/types.h>
#include <sys/un.h>
#include <syslog.h>
#include <unistd.h>

#include "../common/cras_file_wait.h"
#include "../common/cras_observer_ops.h"
#include "../common/cras_string.h"
#include "cras_config.h"
#include "cras_messages.h"
#include "cras_shm.h"
#include "cras_types.h"
#include "cras_util.h"
#include "third_party/utlist/utlist.h"

static const size_t MAX_CMD_MSG_LEN = 256;
static const size_t SERVER_SHUTDOWN_TIMEOUT_US = 500000;
static const size_t SERVER_CONNECT_TIMEOUT_MS = 1000;
static const size_t HOTWORD_FRAME_RATE = 16000;
static const size_t HOTWORD_BLOCK_SIZE = 320;

// Commands sent from the user to the running client.
enum {
  CLIENT_STOP,
  CLIENT_ADD_STREAM,
  CLIENT_REMOVE_STREAM,
  CLIENT_SET_AEC_REF,
  CLIENT_SET_STREAM_VOLUME_SCALER,
  CLIENT_SERVER_CONNECT,
  CLIENT_SERVER_CONNECT_ASYNC,
};

struct command_msg {
  unsigned len;
  unsigned msg_id;
  cras_stream_id_t stream_id;
};

struct set_stream_volume_command_message {
  struct command_msg header;
  float volume_scaler;
};

// Command to set AEC reference to given stream.
struct set_aec_ref_command_message {
  struct command_msg header;
  uint32_t dev_idx;
};

// Adds a stream to the client.
struct add_stream_command_message {
  struct command_msg header;
  // The stream to add.
  struct client_stream* stream;
  // Filled with the stream id of the new stream.
  cras_stream_id_t* stream_id_out;
  // Index of the device to attach the newly created stream.
  // NO_DEVICE means not to pin the stream to a device.
  uint32_t dev_idx;
};

// Commands send from a running stream to the client.
enum {
  CLIENT_STREAM_EOF,
};

struct stream_msg {
  unsigned msg_id;
  cras_stream_id_t stream_id;
};

enum CRAS_THREAD_STATE {
  CRAS_THREAD_STOP,
  // Isn't (shouldn't be) running.
  CRAS_THREAD_WARMUP,
  /* Is started, but not fully functional: waiting
   * for resources to be ready for example. */
  CRAS_THREAD_RUNNING,
  // Is running and fully functional.
};

// Manage information for a thread.
struct thread_state {
  pthread_t tid;
  enum CRAS_THREAD_STATE state;
};

/* Parameters used when setting up a capture or playback stream. See comment
 * above cras_client_stream_params_create or libcras_stream_params_set in the
 * header for descriptions. */
struct cras_stream_params {
  enum CRAS_STREAM_DIRECTION direction;
  size_t buffer_frames;
  size_t cb_threshold;
  enum CRAS_STREAM_TYPE stream_type;
  enum CRAS_CLIENT_TYPE client_type;
  uint32_t flags;
  uint64_t effects;
  void* user_data;
  cras_playback_cb_t aud_cb;
  cras_unified_cb_t unified_cb;
  cras_error_cb_t err_cb;
  struct cras_audio_format format;
  libcras_stream_cb_t stream_cb;
};

// Represents an attached audio stream.
struct client_stream {
  // Unique stream identifier.
  cras_stream_id_t id;
  // After server connects audio messages come in here.
  int aud_fd;  // audio messages from server come in here.
  // playback, capture, or loopback (see CRAS_STREAM_DIRECTION).
  enum CRAS_STREAM_DIRECTION direction;
  // Currently only used for CRAS_INPUT_STREAM_FLAG.
  uint32_t flags;
  // Amount to scale the stream by, 0.0 to 1.0. Client could
  // change this scaler value before stream actually connected, so we need
  // to cache it until shm is prepared and apply it.
  float volume_scaler;
  struct thread_state thread;
  // Pipe to wake the audio thread.
  int wake_fds[2];  // Pipe to wake the thread
  // The client this stream is attached to.
  struct cras_client* client;
  // Audio stream configuration.
  struct cras_stream_params* config;
  // Shared memory used to exchange audio samples with the server.
  struct cras_audio_shm* shm;
  // Form a linked list of streams attached to a client.
  struct client_stream *prev, *next;
};

// State of the socket.
typedef enum cras_socket_state {
  CRAS_SOCKET_STATE_DISCONNECTED,
  /* Not connected. Also used to cleanup the current connection
   * before restarting the connection attempt. */
  CRAS_SOCKET_STATE_WAIT_FOR_SOCKET,
  /* Waiting for the socket file to exist. Socket file existence
   * is monitored using cras_file_wait. */
  CRAS_SOCKET_STATE_WAIT_FOR_WRITABLE,
  // Waiting for the socket to have something at the other end.
  CRAS_SOCKET_STATE_FIRST_MESSAGE,
  /* Waiting for the first messages from the server and set our
   * client ID. */
  CRAS_SOCKET_STATE_CONNECTED,
  // The socket is connected and working.
  CRAS_SOCKET_STATE_ERROR_DELAY,
  /* There was an error during one of the above states. Sleep for
   * a bit before continuing. If this state could not be initiated
   * then we move to the DISCONNECTED state and notify via the
   * connection callback. */
} cras_socket_state_t;

// An in-flight |CRAS_SERVER_REQUEST_FLOOP| request
struct floop_request {
  // mutex protecting the members
  pthread_mutex_t mu;
  // whether the response is fullfilled. signalled by request_floop_ready.
  pthread_cond_t cond;
  // whether the response is fullfilled. set by request_floop_ready
  bool fullfilled;
  // return value of the request
  int32_t response;
  // pointers for ulist.h
  struct floop_request *prev, *next;
};

// Represents a client used to communicate with the audio server.
struct cras_client {
  // Unique identifier for this client, negative until connected.
  int id;
  // Incoming messages from server.
  int server_fd;
  // State of the server's socket.
  cras_socket_state_t server_fd_state;
  // Eventfd to wait on until a connection is established.
  int server_event_fd;
  // Pipe for attached streams.
  int stream_fds[2];
  // Pipe for user commands to thread.
  int command_fds[2];
  // Pipe for acking/nacking command messages from thread.
  int command_reply_fds[2];
  // Server communication socket file.
  const char* sock_file;
  // Structure used to monitor existence of the socket file.
  struct cras_file_wait* sock_file_wait;
  // Set to true when the socket file exists.
  bool sock_file_exists;
  struct thread_state thread;
  // ID to give the next stream.
  cras_stream_id_t next_stream_id;
  // Condition used during stream startup.
  pthread_cond_t stream_start_cond;
  // Lock used during stream startup.
  pthread_mutex_t stream_start_lock;
  // Passes back the result of the last user command.
  int last_command_result;
  // Linked list of streams attached to this client.
  struct client_stream* streams;
  // RO shared memory region holding server state.
  const struct cras_server_state* server_state;
  // RO shared memory region holding audio thread log.
  struct audio_thread_event_log* atlog_ro;
  // Function to call when debug info is received.
  void (*debug_info_callback)(struct cras_client*);
  // Function to call when atlog RO fd is received.
  void (*atlog_access_callback)(struct cras_client*);
  // Function to call when hotword models info is ready.
  get_hotword_models_cb_t get_hotword_models_cb;
  // Function to called when a connection state changes.
  cras_connection_status_cb_t server_connection_cb;
  // User argument for server_connection_cb.
  void* server_connection_user_arg;
  // Function to call for setting audio thread priority.
  cras_thread_priority_cb_t thread_priority_cb;
  // Functions to call when system state changes.
  struct cras_observer_ops observer_ops;
  // Context passed to client in state change callbacks.
  void* observer_context;
  struct floop_request* floop_request_list;
  pthread_mutex_t floop_request_list_mu;
  // Client type set directly on the client by cras_client_set_client_type.
  enum CRAS_CLIENT_TYPE client_type;
};

/*
 * Holds the client pointer plus internal book keeping.
 */
struct client_int {
  // The client
  struct cras_client client;
  // lock to make the client's server_state thread-safe.
  pthread_rwlock_t server_state_rwlock;
};

#define to_client_int(cptr) \
  ((struct client_int*)((char*)cptr - offsetof(struct client_int, client)))

/*
 * Holds the hotword stream format, params, and ID used when waiting for a
 * hotword. The structure is created by cras_client_enable_hotword_callback and
 * destroyed by cras_client_disable_hotword_callback.
 */
struct cras_hotword_handle {
  struct cras_audio_format* format;
  struct cras_stream_params* params;
  cras_stream_id_t stream_id;
  cras_hotword_trigger_cb_t trigger_cb;
  cras_hotword_error_cb_t err_cb;
  void* user_data;
};

struct cras_stream_cb_data {
  cras_stream_id_t stream_id;
  enum CRAS_STREAM_DIRECTION direction;
  uint8_t* buf;
  unsigned int frames;
  uint32_t overrun_frames;
  struct timespec dropped_samples_duration;
  struct timespec underrun_duration;
  struct timespec sample_ts;
  void* user_arg;
};

int stream_cb_get_stream_id(struct cras_stream_cb_data* data,
                            cras_stream_id_t* id) {
  *id = data->stream_id;
  return 0;
}

int stream_cb_get_buf(struct cras_stream_cb_data* data, uint8_t** buf) {
  *buf = data->buf;
  return 0;
}

int stream_cb_get_frames(struct cras_stream_cb_data* data,
                         unsigned int* frames) {
  *frames = data->frames;
  return 0;
}

int stream_cb_get_overrun_frames(struct cras_stream_cb_data* data,
                                 unsigned int* frames) {
  *frames = data->overrun_frames;
  return 0;
}

int stream_cb_get_dropped_samples_duration(struct cras_stream_cb_data* data,
                                           struct timespec* duration) {
  *duration = data->dropped_samples_duration;
  return 0;
}

int stream_cb_get_underrun_duration(struct cras_stream_cb_data* data,
                                    struct timespec* duration) {
  *duration = data->underrun_duration;
  return 0;
}

int stream_cb_get_latency(struct cras_stream_cb_data* data,
                          struct timespec* latency) {
  if (data->direction == CRAS_STREAM_INPUT) {
    cras_client_calc_capture_latency(&data->sample_ts, latency);
  } else {
    cras_client_calc_playback_latency(&data->sample_ts, latency);
  }
  return 0;
}

int stream_cb_get_user_arg(struct cras_stream_cb_data* data, void** user_arg) {
  *user_arg = data->user_arg;
  return 0;
}

struct libcras_stream_cb_data* libcras_stream_cb_data_create(
    cras_stream_id_t stream_id,
    enum CRAS_STREAM_DIRECTION direction,
    uint8_t* buf,
    unsigned int frames,
    unsigned int overrun_frames,
    struct timespec dropped_samples_duration,
    struct timespec underrun_duration,
    struct timespec sample_ts,
    void* user_arg) {
  struct libcras_stream_cb_data* data = (struct libcras_stream_cb_data*)calloc(
      1, sizeof(struct libcras_stream_cb_data));
  if (!data) {
    syslog(LOG_ERR, "cras_client: calloc: %s", cras_strerror(errno));
    return NULL;
  }
  data->data_ = (struct cras_stream_cb_data*)calloc(
      1, sizeof(struct cras_stream_cb_data));
  if (!data->data_) {
    syslog(LOG_ERR, "cras_client: calloc: %s", cras_strerror(errno));
    free(data);
    return NULL;
  }
  data->api_version = CRAS_API_VERSION;
  data->get_stream_id = stream_cb_get_stream_id;
  data->get_buf = stream_cb_get_buf;
  data->get_frames = stream_cb_get_frames;
  data->get_overrun_frames = stream_cb_get_overrun_frames;
  data->get_dropped_samples_duration = stream_cb_get_dropped_samples_duration;
  data->get_underrun_duration = stream_cb_get_underrun_duration;
  data->get_latency = stream_cb_get_latency;
  data->get_user_arg = stream_cb_get_user_arg;
  data->data_->stream_id = stream_id;
  data->data_->direction = direction;
  data->data_->buf = buf;
  data->data_->frames = frames;
  data->data_->overrun_frames = overrun_frames;
  data->data_->dropped_samples_duration = dropped_samples_duration;
  data->data_->underrun_duration = underrun_duration;
  data->data_->sample_ts = sample_ts;
  data->data_->user_arg = user_arg;
  return data;
}

void libcras_stream_cb_data_destroy(struct libcras_stream_cb_data* data) {
  if (data) {
    free(data->data_);
  }
  free(data);
}

/*
 * Local Helpers
 */

static int client_thread_rm_stream(struct cras_client* client,
                                   cras_stream_id_t stream_id);
static int handle_message_from_server(struct cras_client* client);
static int reregister_notifications(struct cras_client* client);

static struct libcras_node_info* libcras_node_info_create(
    struct cras_iodev_info* iodev,
    struct cras_ionode_info* ionode);

/*
 * Unlock the server_state_rwlock if lock_rc is 0.
 *
 * Args:
 *    client - The CRAS client pointer.
 *    lock_rc - The result of server_state_rdlock or
 *              server_state_wrlock.
 */
static void server_state_unlock(const struct cras_client* client, int lock_rc) {
  struct client_int* client_int;

  if (!client) {
    return;
  }
  client_int = to_client_int(client);
  if (lock_rc == 0) {
    pthread_rwlock_unlock(&client_int->server_state_rwlock);
  }
}

/*
 * Lock the server_state_rwlock for reading.
 *
 * Also checks that the server_state pointer is valid.
 *
 * Args:
 *    client - The CRAS client pointer.
 * Returns:
 *    0 for success, positive error code on error.
 *    Returns EINVAL if the server state pointer is NULL.
 */
static int server_state_rdlock(const struct cras_client* client) {
  struct client_int* client_int;
  int lock_rc;

  if (!client) {
    return EINVAL;
  }
  client_int = to_client_int(client);
  lock_rc = pthread_rwlock_rdlock(&client_int->server_state_rwlock);
  if (lock_rc != 0) {
    return lock_rc;
  }
  if (!client->server_state) {
    pthread_rwlock_unlock(&client_int->server_state_rwlock);
    return EINVAL;
  }
  return 0;
}

/*
 * Lock the server_state_rwlock for writing.
 *
 * Args:
 *    client - The CRAS client pointer.
 * Returns:
 *    0 for success, positive error code on error.
 */
static int server_state_wrlock(const struct cras_client* client) {
  struct client_int* client_int;

  if (!client) {
    return EINVAL;
  }
  client_int = to_client_int(client);
  return pthread_rwlock_wrlock(&client_int->server_state_rwlock);
}

// Get the stream pointer from a stream id.
static struct client_stream* stream_from_id(const struct cras_client* client,
                                            unsigned int id) {
  struct client_stream* out;

  DL_SEARCH_SCALAR(client->streams, out, id, id);
  return out;
}

/*
 * Fill a pollfd structure with the current server fd and events.
 */
void server_fill_pollfd(const struct cras_client* client,
                        struct pollfd* poll_fd) {
  int events = 0;

  poll_fd->fd = client->server_fd;
  switch (client->server_fd_state) {
    case CRAS_SOCKET_STATE_DISCONNECTED:
      break;
    case CRAS_SOCKET_STATE_WAIT_FOR_SOCKET:
    case CRAS_SOCKET_STATE_FIRST_MESSAGE:
    case CRAS_SOCKET_STATE_CONNECTED:
    case CRAS_SOCKET_STATE_ERROR_DELAY:
      events = POLLIN;
      break;
    case CRAS_SOCKET_STATE_WAIT_FOR_WRITABLE:
      events = POLLOUT;
      break;
  }
  poll_fd->events = events;
  poll_fd->revents = 0;
}

/*
 * Change the server_fd_state.
 */
static void server_fd_move_to_state(struct cras_client* client,
                                    cras_socket_state_t state) {
  if (state == client->server_fd_state) {
    return;
  }

  client->server_fd_state = state;
}

/*
 * Action to take when in state ERROR_DELAY.
 *
 * In this state we want to sleep for a few seconds before retrying the
 * connection to the audio server.
 *
 * If server_fd is negative: create a timer and setup server_fd with the
 * timer's fd. If server_fd is not negative and there is input, then assume
 * that the timer has expired, and restart the connection by moving to
 * WAIT_FOR_SOCKET state.
 */
static int error_delay_next_action(struct cras_client* client,
                                   int poll_revents) {
  int rc;
  struct itimerspec timeout;

  if (client->server_fd == -1) {
    client->server_fd =
        timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
    if (client->server_fd == -1) {
      rc = -errno;
      syslog(LOG_ERR, "cras_client: Could not create timerfd: %s",
             cras_strerror(-rc));
      return rc;
    }

    // Setup a relative timeout of 2 seconds.
    memset(&timeout, 0, sizeof(timeout));
    timeout.it_value.tv_sec = 2;
    rc = timerfd_settime(client->server_fd, 0, &timeout, NULL);
    if (rc != 0) {
      rc = -errno;
      syslog(LOG_ERR, "cras_client: Could not set timeout: %s",
             cras_strerror(-rc));
      return rc;
    }
    return 0;
  } else if ((poll_revents & POLLIN) == 0) {
    return 0;
  }

  // Move to the next state: close the timer fd first.
  close(client->server_fd);
  client->server_fd = -1;
  server_fd_move_to_state(client, CRAS_SOCKET_STATE_WAIT_FOR_SOCKET);
  return 0;
}

/*
 * Action to take when in WAIT_FOR_SOCKET state.
 *
 * In this state we are waiting for the socket file to exist. The existence of
 * the socket file is continually monitored using the cras_file_wait structure
 * and a separate fd. When the sock_file_exists boolean is modified, the state
 * machine is invoked.
 *
 * If the socket file exists, then we move to the WAIT_FOR_WRITABLE state.
 */
static void wait_for_socket_next_action(struct cras_client* client) {
  if (client->sock_file_exists) {
    server_fd_move_to_state(client, CRAS_SOCKET_STATE_WAIT_FOR_WRITABLE);
  }
}

/*
 * Action to take when in WAIT_FOR_WRITABLE state.
 *
 * In this state we are initiating a connection the server and waiting for the
 * server to ready for incoming messages.
 *
 * Create the socket to the server, and wait while a connect request results in
 * -EINPROGRESS. Otherwise, we assume that the socket file will be deleted by
 * the server and the server_fd_state will be changed in
 * sock_file_wait_dispatch().
 */
static int wait_for_writable_next_action(struct cras_client* client,
                                         int poll_revents) {
  int rc;
  struct sockaddr_un address;

  if (client->server_fd == -1) {
    client->server_fd = socket(PF_UNIX, SOCK_SEQPACKET, 0);
    if (client->server_fd < 0) {
      rc = -errno;
      syslog(LOG_WARNING, "cras_client: server socket failed: %s",
             cras_strerror(-rc));
      return rc;
    }
  } else if ((poll_revents & POLLOUT) == 0) {
    return 0;
  }

  /* We make the file descriptor non-blocking when we do connect(), so we
   * don't block indefinitely. */
  cras_make_fd_nonblocking(client->server_fd);

  memset(&address, 0, sizeof(struct sockaddr_un));
  address.sun_family = AF_UNIX;
  strlcpy(address.sun_path, client->sock_file, sizeof(address.sun_path));
  rc = connect(client->server_fd, (struct sockaddr*)&address,
               sizeof(struct sockaddr_un));
  if (rc != 0) {
    rc = -errno;
    /* For -EINPROGRESS, we wait for POLLOUT on the server_fd.
     * Otherwise CRAS is not running and we assume that the socket
     * file will be deleted and recreated. Notification of that will
     * happen via the sock_file_wait_dispatch(). */
    if (rc == -ECONNREFUSED) {
      /* CRAS is not running, don't log this error and just
       * stay in this state waiting sock_file_wait_dispatch()
       * to move the state machine. */
      close(client->server_fd);
      client->server_fd = -1;
    } else if (rc != -EINPROGRESS) {
      syslog(LOG_WARNING, "cras_client: server connect failed: %s",
             cras_strerror(-rc));
      return rc;
    }
    return 0;
  }

  cras_make_fd_blocking(client->server_fd);
  server_fd_move_to_state(client, CRAS_SOCKET_STATE_FIRST_MESSAGE);
  return 0;
}

/*
 * Action to take when transitioning to the CONNECTED state.
 */
static int connect_transition_action(struct cras_client* client) {
  eventfd_t event_value;
  int rc;

  rc = reregister_notifications(client);
  if (rc < 0) {
    return rc;
  }

  server_fd_move_to_state(client, CRAS_SOCKET_STATE_CONNECTED);
  /* Notify anyone waiting on this state change that we're
   * connected. */
  eventfd_read(client->server_event_fd, &event_value);
  eventfd_write(client->server_event_fd, 1);
  if (client->server_connection_cb) {
    client->server_connection_cb(client, CRAS_CONN_STATUS_CONNECTED,
                                 client->server_connection_user_arg);
  }
  return 0;
}

/*
 * Action to take when in the FIRST_MESSAGE state.
 *
 * We are waiting for the first message from the server. When our client ID has
 * been set, then we can move to the CONNECTED state.
 */
static int first_message_next_action(struct cras_client* client,
                                     int poll_revents) {
  int rc;

  if (client->server_fd < 0) {
    return -EINVAL;
  }

  if ((poll_revents & POLLIN) == 0) {
    return 0;
  }

  rc = handle_message_from_server(client);
  if (rc < 0) {
    syslog(LOG_WARNING, "handle first message: %s", cras_strerror(-rc));
  } else if (client->id >= 0) {
    rc = connect_transition_action(client);
  } else {
    syslog(LOG_WARNING, "did not get ID after first message!");
    rc = -EINVAL;
  }
  return rc;
}

/*
 * Play nice and shutdown the server socket.
 */
static inline int shutdown_and_close_socket(int sockfd) {
  int rc;
  uint8_t buffer[CRAS_CLIENT_MAX_MSG_SIZE];
  struct timeval tv;

  tv.tv_sec = 0;
  tv.tv_usec = SERVER_SHUTDOWN_TIMEOUT_US;
  setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, (char*)&tv, sizeof(tv));

  rc = shutdown(sockfd, SHUT_WR);
  if (rc < 0) {
    return rc;
  }
  // Wait until the socket is closed by the peer.
  for (;;) {
    rc = recv(sockfd, buffer, sizeof(buffer), 0);
    if (rc <= 0) {
      break;
    }
  }
  return close(sockfd);
}

/*
 * Action to take when disconnecting from the server.
 *
 * Clean up the server socket, and the server_state pointer. Move to the next
 * logical state.
 */
static void disconnect_transition_action(struct cras_client* client,
                                         bool force) {
  eventfd_t event_value;
  cras_socket_state_t old_state = client->server_fd_state;
  struct client_stream* s;
  int lock_rc;

  /* Stop all playing streams.
   * TODO(muirj): Pause and resume streams. */
  DL_FOREACH (client->streams, s) {
    s->config->err_cb(client, s->id, -ENOTCONN, s->config->user_data);
    client_thread_rm_stream(client, s->id);
  }

  // Clean up the server_state pointer.
  lock_rc = server_state_wrlock(client);
  if (client->server_state) {
    munmap((void*)client->server_state, sizeof(*client->server_state));
    client->server_state = NULL;
  }
  server_state_unlock(client, lock_rc);

  // Our ID is unknown now.
  client->id = -1;

  // Clean up the server fd.
  if (client->server_fd >= 0) {
    if (!force) {
      shutdown_and_close_socket(client->server_fd);
    } else {
      close(client->server_fd);
    }
    client->server_fd = -1;
  }

  /* Reset the server_event_fd value to 0 (and cause subsequent threads
   * waiting on the connection to wait). */
  eventfd_read(client->server_event_fd, &event_value);

  switch (old_state) {
    case CRAS_SOCKET_STATE_DISCONNECTED:
      // Do nothing: already disconnected.
      break;
    case CRAS_SOCKET_STATE_ERROR_DELAY:
      /* We're disconnected and there was a failure to setup
       * automatic reconnection, so call the server error
       * callback now. */
      server_fd_move_to_state(client, CRAS_SOCKET_STATE_DISCONNECTED);
      if (client->server_connection_cb) {
        client->server_connection_cb(client, CRAS_CONN_STATUS_FAILED,
                                     client->server_connection_user_arg);
      }
      break;
    case CRAS_SOCKET_STATE_WAIT_FOR_SOCKET:
    case CRAS_SOCKET_STATE_WAIT_FOR_WRITABLE:
    case CRAS_SOCKET_STATE_FIRST_MESSAGE:
      /* We are running this state transition while a connection is
       * in progress for an error case. When there is no error, we
       * come into this function in the DISCONNECTED state. */
      server_fd_move_to_state(client, CRAS_SOCKET_STATE_ERROR_DELAY);
      break;
    case CRAS_SOCKET_STATE_CONNECTED:
      /* Disconnected from CRAS (for an error), wait for the socket
       * file to be (re)created. */
      server_fd_move_to_state(client, CRAS_SOCKET_STATE_WAIT_FOR_SOCKET);
      // Notify the caller that we aren't connected anymore.
      if (client->server_connection_cb) {
        client->server_connection_cb(client, CRAS_CONN_STATUS_DISCONNECTED,
                                     client->server_connection_user_arg);
      }
      break;
  }
}

static int server_fd_dispatch(struct cras_client* client, int poll_revents) {
  int rc = 0;
  cras_socket_state_t old_state;

  if ((poll_revents & POLLHUP) != 0) {
    // Error or disconnect: cleanup and make a state change now.
    disconnect_transition_action(client, true);
  }
  old_state = client->server_fd_state;

  switch (client->server_fd_state) {
    case CRAS_SOCKET_STATE_DISCONNECTED:
      // Assume that we've taken the necessary actions.
      return -ENOTCONN;
    case CRAS_SOCKET_STATE_ERROR_DELAY:
      rc = error_delay_next_action(client, poll_revents);
      break;
    case CRAS_SOCKET_STATE_WAIT_FOR_SOCKET:
      wait_for_socket_next_action(client);
      break;
    case CRAS_SOCKET_STATE_WAIT_FOR_WRITABLE:
      rc = wait_for_writable_next_action(client, poll_revents);
      break;
    case CRAS_SOCKET_STATE_FIRST_MESSAGE:
      rc = first_message_next_action(client, poll_revents);
      break;
    case CRAS_SOCKET_STATE_CONNECTED:
      if ((poll_revents & POLLIN) != 0) {
        rc = handle_message_from_server(client);
      }
      break;
  }

  if (rc != 0) {
    // If there is an error, then start-over.
    rc = server_fd_dispatch(client, POLLHUP);
  } else if (old_state != client->server_fd_state) {
    // There was a state change, process the new state now.
    rc = server_fd_dispatch(client, 0);
  }
  return rc;
}

/*
 * Start connecting to the server if we aren't already.
 */
static int server_connect(struct cras_client* client) {
  if (client->server_fd_state != CRAS_SOCKET_STATE_DISCONNECTED) {
    return 0;
  }
  // Start waiting for the server socket to exist.
  server_fd_move_to_state(client, CRAS_SOCKET_STATE_WAIT_FOR_SOCKET);
  return server_fd_dispatch(client, 0);
}

/*
 * Disconnect from the server if we haven't already.
 */
static void server_disconnect(struct cras_client* client) {
  if (client->server_fd_state == CRAS_SOCKET_STATE_DISCONNECTED) {
    return;
  }
  /* Set the disconnected state first so that the disconnect
   * transition doesn't move the server state to ERROR_DELAY. */
  server_fd_move_to_state(client, CRAS_SOCKET_STATE_DISCONNECTED);
  disconnect_transition_action(client, false);
}

/*
 * Called when something happens to the socket file.
 */
static void sock_file_wait_callback(void* context,
                                    cras_file_wait_event_t event,
                                    const char* filename) {
  struct cras_client* client = (struct cras_client*)context;
  switch (event) {
    case CRAS_FILE_WAIT_EVENT_CREATED:
      client->sock_file_exists = 1;
      switch (client->server_fd_state) {
        case CRAS_SOCKET_STATE_DISCONNECTED:
        case CRAS_SOCKET_STATE_ERROR_DELAY:
        case CRAS_SOCKET_STATE_FIRST_MESSAGE:
        case CRAS_SOCKET_STATE_CONNECTED:
          break;
        case CRAS_SOCKET_STATE_WAIT_FOR_SOCKET:
        case CRAS_SOCKET_STATE_WAIT_FOR_WRITABLE:
          /* The socket file exists. Tell the server state
           * machine. */
          server_fd_dispatch(client, 0);
          break;
      }
      break;
    case CRAS_FILE_WAIT_EVENT_DELETED:
      client->sock_file_exists = 0;
      switch (client->server_fd_state) {
        case CRAS_SOCKET_STATE_DISCONNECTED:
          break;
        case CRAS_SOCKET_STATE_WAIT_FOR_SOCKET:
        case CRAS_SOCKET_STATE_WAIT_FOR_WRITABLE:
        case CRAS_SOCKET_STATE_ERROR_DELAY:
        case CRAS_SOCKET_STATE_FIRST_MESSAGE:
        case CRAS_SOCKET_STATE_CONNECTED:
          // Restart the connection process.
          server_disconnect(client);
          server_connect(client);
          break;
      }
      break;
    case CRAS_FILE_WAIT_EVENT_NONE:
      break;
  }
}

/*
 * Service the sock_file_wait's fd.
 *
 * If the socket file is deleted, then cause a disconnect from the server.
 * Otherwise, start a reconnect depending on the server_fd_state.
 */
static int sock_file_wait_dispatch(struct cras_client* client,
                                   int poll_revents) {
  int rc;

  if ((poll_revents & POLLIN) == 0) {
    return 0;
  }

  rc = cras_file_wait_dispatch(client->sock_file_wait);
  if (rc == -EAGAIN || rc == -EWOULDBLOCK) {
    rc = 0;
  } else if (rc != 0) {
    syslog(LOG_WARNING, "cras_file_wait_dispatch: %s", cras_strerror(-rc));
  }
  return rc;
}

/*
 * Waits until we have heard back from the server so that we know we are
 * connected.
 *
 * The connected success/failure message is always the first message the server
 * sends. Return non zero if client is connected to the server. A return code
 * of zero means that the client is not connected to the server.
 */
static int check_server_connected_wait(struct cras_client* client,
                                       struct timespec* timeout) {
  int rc = 0;
  struct pollfd poll_fd;

  poll_fd.fd = client->server_event_fd;
  poll_fd.events = POLLIN;
  poll_fd.revents = 0;

  /* The server_event_fd is only read and written by the functions
   * that connect to the server. When a connection is established the
   * eventfd has a value of 1 and cras_poll will return immediately
   * with 1. When there is no connection to the server, then this
   * function waits until the timeout has expired or a non-zero value
   * is written to the server_event_fd. */
  while (rc == 0) {
    rc = cras_poll(&poll_fd, 1, timeout, NULL);
  }
  return rc > 0;
}

// Returns non-zero if the thread is running (not stopped).
static inline int thread_is_running(struct thread_state* thread) {
  return thread->state != CRAS_THREAD_STOP;
}

/*
 * Opens the server socket and connects to it.
 * Args:
 *    client - Client pointer created with cras_client_create().
 *    timeout - Connection timeout.
 * Returns:
 *    0 for success, negative error code on failure.
 */
static int connect_to_server(struct cras_client* client,
                             struct timespec* timeout,
                             bool use_command_thread) {
  int rc;
  struct pollfd poll_fd[2];
  struct timespec connected_timeout;

  if (!client) {
    return -EINVAL;
  }

  if (thread_is_running(&client->thread) && use_command_thread) {
    rc = cras_client_connect_async(client);
    if (rc == 0) {
      rc = check_server_connected_wait(client, timeout);
      return rc ? 0 : -ESHUTDOWN;
    }
  }

  connected_timeout.tv_sec = 0;
  connected_timeout.tv_nsec = 0;
  if (check_server_connected_wait(client, &connected_timeout)) {
    return 0;
  }

  poll_fd[0].fd = cras_file_wait_get_fd(client->sock_file_wait);
  poll_fd[0].events = POLLIN;

  rc = server_connect(client);
  while (rc == 0) {
    // Wait until we've connected or until there is a timeout.
    // Meanwhile handle incoming actions on our fds.

    server_fill_pollfd(client, &(poll_fd[1]));
    rc = cras_poll(poll_fd, 2, timeout, NULL);
    if (rc <= 0) {
      continue;
    }

    if (poll_fd[0].revents) {
      rc = sock_file_wait_dispatch(client, poll_fd[0].revents);
      continue;
    }

    if (poll_fd[1].revents) {
      rc = server_fd_dispatch(client, poll_fd[1].revents);
      if (rc == 0 && client->server_fd_state == CRAS_SOCKET_STATE_CONNECTED) {
        break;
      }
    }
  }

  if (rc != 0) {
    syslog(LOG_WARNING, "cras_client: Connect server failed: %s",
           cras_strerror(-rc));
  }

  return rc;
}

static int connect_to_server_wait_retry(struct cras_client* client,
                                        int timeout_ms,
                                        bool use_command_thread) {
  struct timespec timeout_value;
  struct timespec* timeout;

  if (timeout_ms < 0) {
    timeout = NULL;
  } else {
    timeout = &timeout_value;
    ms_to_timespec(timeout_ms, timeout);
  }

  /* If connected, wait for the first message from the server
   * indicating it's ready. */
  return connect_to_server(client, timeout, use_command_thread);
}

/*
 * Tries to connect to the server.  Waits for the initial message from the
 * server.  This will happen near instantaneously if the server is already
 * running.
 */
static int connect_to_server_wait(struct cras_client* client,
                                  bool use_command_thread) {
  return connect_to_server_wait_retry(client, SERVER_CONNECT_TIMEOUT_MS,
                                      use_command_thread);
}

/*
 * Audio thread.
 */

/* Sends a message from the stream to the client to indicate an error.
 * If the running stream encounters an error, then it must tell the client
 * to stop running it.
 */
static int send_stream_message(const struct client_stream* stream,
                               unsigned msg_id) {
  int res;
  struct stream_msg msg;

  msg.stream_id = stream->id;
  msg.msg_id = msg_id;
  res = write(stream->client->stream_fds[1], &msg, sizeof(msg));
  if (res != sizeof(msg)) {
    return -EPIPE;
  }

  return 0;
}

/* Blocks until there is data to be read from the read_fd or until woken by an
 * incoming "poke" on wake_fd. Up to "len" bytes are read into "buf". */
static int read_with_wake_fd(int wake_fd,
                             int read_fd,
                             uint8_t* buf,
                             size_t len) {
  struct pollfd pollfds[2];
  int nread = 0;
  int nfds = 1;
  int rc;
  char tmp;

  pollfds[0].fd = wake_fd;
  pollfds[0].events = POLLIN;
  if (read_fd >= 0) {
    nfds++;
    pollfds[1].fd = read_fd;
    pollfds[1].events = POLLIN;
  }

  rc = poll(pollfds, nfds, -1);
  if (rc < 0) {
    return rc;
  }
  if (read_fd >= 0 && pollfds[1].revents & POLLIN) {
    nread = read(read_fd, buf, len);
    if (nread != (int)len) {
      return -EIO;
    }
  }
  if (pollfds[0].revents & POLLIN) {
    rc = read(wake_fd, &tmp, 1);
    if (rc < 0) {
      return rc;
    }
  }

  return nread;
}
/* Check the availability and configures a capture buffer.
 * Args:
 *     stream - The input stream to configure buffer for.
 *     captured_frames - To be filled with the pointer to the beginning of
 *         captured buffer.
 *     num_frames - Number of captured frames.
 * Returns:
 *     Number of frames available in captured_frames.
 */
static unsigned int config_capture_buf(struct client_stream* stream,
                                       uint8_t** captured_frames,
                                       unsigned int num_frames) {
  /* Always return the beginning of the read buffer because Chrome expects
   * so. */
  *captured_frames = cras_shm_get_read_buffer_base(stream->shm);

  // Don't ask for more frames than the client desires.
  if (stream->flags & BULK_AUDIO_OK) {
    num_frames = MIN(num_frames, stream->config->buffer_frames);
  } else {
    num_frames = MIN(num_frames, stream->config->cb_threshold);
  }

  /* If shm readable frames is less than client requests, that means
   * overrun has happened in server side. Don't send partial corrupted
   * buffer to client. */
  if (cras_shm_get_curr_read_frames(stream->shm) < num_frames) {
    return 0;
  }

  return num_frames;
}

static void complete_capture_read_current(struct client_stream* stream,
                                          unsigned int num_frames) {
  cras_shm_buffer_read_current(stream->shm, num_frames);
}

static int send_capture_reply(struct client_stream* stream,
                              unsigned int frames,
                              int err) {
  struct audio_message aud_msg;
  int rc;

  if (!cras_stream_uses_input_hw(stream->direction)) {
    return 0;
  }

  aud_msg.id = AUDIO_MESSAGE_DATA_CAPTURED;
  aud_msg.frames = frames;
  aud_msg.error = err;

  rc = write(stream->aud_fd, &aud_msg, sizeof(aud_msg));
  if (rc != sizeof(aud_msg)) {
    return -EPIPE;
  }

  return 0;
}

/* For capture streams this handles the message signalling that data is ready to
 * be passed to the user of this stream.  Calls the audio callback with the new
 * samples, and mark them as read.
 * Args:
 *    stream - The stream the message was received for.
 *    num_frames - The number of captured frames.
 * Returns:
 *    0, unless there is a fatal error or the client declares enod of file.
 */
static int handle_capture_data_ready(struct client_stream* stream,
                                     unsigned int num_frames) {
  int frames;
  struct cras_stream_params* config;
  uint8_t* captured_frames;
  struct timespec ts;
  struct timespec dropped_samples_duration;
  struct timespec underrun_duration;
  int rc = 0;
  struct libcras_stream_cb_data* data;

  config = stream->config;
  // If this message is for an output stream, log error and drop it.
  if (!cras_stream_has_input(stream->direction)) {
    syslog(LOG_WARNING, "cras_client: Play data to input\n");
    return 0;
  }

  num_frames = config_capture_buf(stream, &captured_frames, num_frames);
  if (num_frames == 0) {
    return 0;
  }

  cras_timespec_to_timespec(&ts, &stream->shm->header->ts);
  cras_timespec_to_timespec(&dropped_samples_duration,
                            &stream->shm->header->dropped_samples_duration);
  cras_timespec_to_timespec(&underrun_duration,
                            &stream->shm->header->underrun_duration);

  if (config->stream_cb) {
    data = libcras_stream_cb_data_create(
        stream->id, stream->direction, captured_frames, num_frames,
        stream->shm->header->overrun_frames, dropped_samples_duration,
        underrun_duration, ts, config->user_data);
    if (!data) {
      return -errno;
    }
    frames = config->stream_cb(data);
    libcras_stream_cb_data_destroy(data);
    data = NULL;
  } else if (config->unified_cb) {
    frames = config->unified_cb(stream->client, stream->id, captured_frames,
                                NULL, num_frames, &ts, NULL, config->user_data);
  } else {
    frames = config->aud_cb(stream->client, stream->id, captured_frames,
                            num_frames, &ts, config->user_data);
  }
  if (frames < 0) {
    send_stream_message(stream, CLIENT_STREAM_EOF);
    rc = frames;
    goto reply_captured;
  }
  if (frames == 0) {
    return 0;
  }

  complete_capture_read_current(stream, frames);
reply_captured:
  return send_capture_reply(stream, frames, rc);
}

// Notifies the server that "frames" samples have been written.
static int send_playback_reply(struct client_stream* stream,
                               unsigned int frames,
                               int error) {
  struct audio_message aud_msg;
  int rc;

  if (!cras_stream_uses_output_hw(stream->direction)) {
    return 0;
  }

  aud_msg.id = AUDIO_MESSAGE_DATA_READY;
  aud_msg.frames = frames;
  aud_msg.error = error;

  rc = write(stream->aud_fd, &aud_msg, sizeof(aud_msg));
  if (rc != sizeof(aud_msg)) {
    return -EPIPE;
  }

  return 0;
}

/* For playback streams when current buffer is empty, this handles the request
 * for more samples by calling the audio callback for the thread, and signaling
 * the server that the samples have been written. */
static int handle_playback_request(struct client_stream* stream,
                                   unsigned int num_frames) {
  uint8_t* buf;
  int frames;
  int rc = 0;
  struct cras_stream_params* config;
  struct cras_audio_shm* shm = stream->shm;
  struct timespec ts;
  struct timespec dropped_samples_duration;
  struct timespec underrun_duration;
  struct libcras_stream_cb_data* data;

  config = stream->config;

  // If this message is for an input stream, log error and drop it.
  if (stream->direction != CRAS_STREAM_OUTPUT) {
    syslog(LOG_WARNING, "cras_client: Record data from output\n");
    return 0;
  }

  buf = cras_shm_get_write_buffer_base(shm);

  // Limit the amount of frames to the configured amount.
  num_frames = MIN(num_frames, config->cb_threshold);

  cras_timespec_to_timespec(&ts, &shm->header->ts);
  cras_timespec_to_timespec(&dropped_samples_duration,
                            &shm->header->dropped_samples_duration);
  cras_timespec_to_timespec(&underrun_duration,
                            &stream->shm->header->underrun_duration);

  // Get samples from the user
  if (config->stream_cb) {
    data = libcras_stream_cb_data_create(
        stream->id, stream->direction, buf, num_frames,
        stream->shm->header->overrun_frames, dropped_samples_duration,
        underrun_duration, ts, config->user_data);
    if (!data) {
      return -errno;
    }
    frames = config->stream_cb(data);
    libcras_stream_cb_data_destroy(data);
    data = NULL;
  } else if (config->unified_cb) {
    frames = config->unified_cb(stream->client, stream->id, NULL, buf,
                                num_frames, NULL, &ts, config->user_data);
  } else {
    frames = config->aud_cb(stream->client, stream->id, buf, num_frames, &ts,
                            config->user_data);
  }
  if (frames < 0) {
    send_stream_message(stream, CLIENT_STREAM_EOF);
    rc = frames;
    goto reply_written;
  }

  cras_shm_buffer_written_start(shm, frames);

reply_written:
  // Signal server that data is ready, or that an error has occurred.
  rc = send_playback_reply(stream, frames, rc);
  return rc;
}

static void audio_thread_set_priority(struct client_stream* stream) {
  // Use provided callback to set priority if available.
  if (stream->client->thread_priority_cb) {
    stream->client->thread_priority_cb(stream->client);
    return;
  }

  // Try to get RT scheduling, if that fails try to set the nice value.
  if (cras_set_rt_scheduling(CRAS_CLIENT_RT_THREAD_PRIORITY) ||
      cras_set_thread_priority(CRAS_CLIENT_RT_THREAD_PRIORITY)) {
    cras_set_nice_level(CRAS_CLIENT_NICENESS_LEVEL);
  }
}

/* Listens to the audio socket for messages from the server indicating that
 * the stream needs to be serviced.  One of these runs per stream. */
static void* audio_thread(void* arg) {
  struct client_stream* stream = (struct client_stream*)arg;
  int thread_terminated = 0;
  struct audio_message aud_msg;
  int aud_fd;
  int num_read;

  if (arg == NULL) {
    return (void*)-EIO;
  }

  audio_thread_set_priority(stream);

  // Notify the control thread that we've started.
  pthread_mutex_lock(&stream->client->stream_start_lock);
  pthread_cond_broadcast(&stream->client->stream_start_cond);
  pthread_mutex_unlock(&stream->client->stream_start_lock);

  while (thread_is_running(&stream->thread) && !thread_terminated) {
    /* While we are warming up, aud_fd may not be valid and some
     * shared memory resources may not yet be available. */
    aud_fd = (stream->thread.state == CRAS_THREAD_WARMUP) ? -1 : stream->aud_fd;
    num_read = read_with_wake_fd(stream->wake_fds[0], aud_fd,
                                 (uint8_t*)&aud_msg, sizeof(aud_msg));
    if (num_read < 0) {
      return (void*)-EIO;
    }
    if (num_read == 0) {
      continue;
    }

    switch (aud_msg.id) {
      case AUDIO_MESSAGE_DATA_READY:
        thread_terminated = handle_capture_data_ready(stream, aud_msg.frames);
        break;
      case AUDIO_MESSAGE_REQUEST_DATA:
        thread_terminated = handle_playback_request(stream, aud_msg.frames);
        break;
      default:
        break;
    }
  }

  return NULL;
}

// Pokes the audio thread so that it can notice if it has been terminated.
static int wake_aud_thread(struct client_stream* stream) {
  char buf[1] = {0};
  int rc;

  rc = write(stream->wake_fds[1], buf, 1);
  if (rc != 1) {
    return rc;
  }
  return 0;
}

/* Stop the audio thread for the given stream.
 * Args:
 *    stream - Stream for which to stop the audio thread.
 *    join - When non-zero, attempt to join the audio thread (wait for it to
 *           complete).
 */
static void stop_aud_thread(struct client_stream* stream, int join) {
  if (thread_is_running(&stream->thread)) {
    stream->thread.state = CRAS_THREAD_STOP;
    wake_aud_thread(stream);
    if (join) {
      pthread_join(stream->thread.tid, NULL);
    }
  }

  if (stream->wake_fds[0] >= 0) {
    close(stream->wake_fds[0]);
    close(stream->wake_fds[1]);
    stream->wake_fds[0] = -1;
  }
}

/* Start the audio thread for this stream.
 * Returns when the thread has started and is waiting.
 * Args:
 *    stream - The stream that needs an audio thread.
 * Returns:
 *    0 for success, or a negative error code.
 */
static int start_aud_thread(struct client_stream* stream) {
  int rc;
  struct timespec future;

  rc = pipe(stream->wake_fds);
  if (rc < 0) {
    rc = -errno;
    syslog(LOG_WARNING, "cras_client: pipe: %s", cras_strerror(-rc));
    return rc;
  }

  stream->thread.state = CRAS_THREAD_WARMUP;

  pthread_mutex_lock(&stream->client->stream_start_lock);
  rc = pthread_create(&stream->thread.tid, NULL, audio_thread, stream);
  if (rc) {
    pthread_mutex_unlock(&stream->client->stream_start_lock);
    syslog(LOG_WARNING, "cras_client: Couldn't create audio stream: %s",
           cras_strerror(rc));
    stream->thread.state = CRAS_THREAD_STOP;
    stop_aud_thread(stream, 0);
    return -rc;
  }

  clock_gettime(CLOCK_REALTIME, &future);
  future.tv_sec += 2;  // Wait up to two seconds.
  rc = pthread_cond_timedwait(&stream->client->stream_start_cond,
                              &stream->client->stream_start_lock, &future);
  pthread_mutex_unlock(&stream->client->stream_start_lock);
  if (rc != 0) {
    /* Something is very wrong: try to cancel the thread and don't
     * wait for it. */
    syslog(LOG_WARNING, "cras_client: Client thread not responding: %s",
           cras_strerror(rc));
    stop_aud_thread(stream, 0);
    return -rc;
  }
  return 0;
}

/*
 * Client thread.
 */

// Gets the update_count of the server state shm region.
static inline unsigned begin_server_state_read(
    const struct cras_server_state* state) {
  unsigned count;

  // Version will be odd when the server is writing.
  while ((count = *(volatile unsigned*)&state->update_count) & 1) {
    sched_yield();
  }
  __sync_synchronize();
  return count;
}

/* Checks if the update count of the server state shm region has changed from
 * count.  Returns 0 if the count still matches.
 */
static inline int end_server_state_read(const struct cras_server_state* state,
                                        unsigned count) {
  __sync_synchronize();
  if (count != *(volatile unsigned*)&state->update_count) {
    return -EAGAIN;
  }
  return 0;
}

// Release shm areas if references to them are held.
static void free_shm(struct client_stream* stream) {
  cras_audio_shm_destroy(stream->shm);
  stream->shm = NULL;
}

/* Handles the stream connected message from the server.  Check if we need a
 * format converter, configure the shared memory region, and start the audio
 * thread that will handle requests from the server. */
static int stream_connected(struct client_stream* stream,
                            const struct cras_client_stream_connected* msg,
                            const int stream_fds[2],
                            const unsigned int num_fds) {
  int rc, samples_prot;
  unsigned int i;
  struct cras_shm_info header_info, samples_info;

  if (msg->err || num_fds != 2) {
    syslog(LOG_WARNING, "cras_client: Error setting up stream %d\n", msg->err);
    rc = msg->err;
    goto err_ret;
  }

  rc = cras_shm_info_init_with_fd(stream_fds[0], cras_shm_header_size(),
                                  &header_info);
  if (rc < 0) {
    goto err_ret;
  }

  rc = cras_shm_info_init_with_fd(stream_fds[1], msg->samples_shm_size,
                                  &samples_info);
  if (rc < 0) {
    cras_shm_info_cleanup(&header_info);
    goto err_ret;
  }

  if (stream->direction == CRAS_STREAM_OUTPUT) {
    samples_prot = PROT_WRITE;
  } else {
    samples_prot = PROT_READ;
  }

  rc = cras_audio_shm_create(&header_info, &samples_info, samples_prot,
                             &stream->shm);
  if (rc < 0) {
    syslog(LOG_WARNING, "cras_client: Error configuring shm");
    goto err_ret;
  }
  cras_shm_copy_shared_config(stream->shm);
  cras_shm_set_volume_scaler(stream->shm, stream->volume_scaler);

  stream->thread.state = CRAS_THREAD_RUNNING;
  wake_aud_thread(stream);

  close(stream_fds[0]);
  close(stream_fds[1]);
  return 0;
err_ret:
  stop_aud_thread(stream, 1);
  for (i = 0; i < num_fds; i++) {
    close(stream_fds[i]);
  }
  free_shm(stream);
  return rc;
}

static int send_connect_message(struct cras_client* client,
                                struct client_stream* stream,
                                uint32_t dev_idx) {
  int rc;
  struct cras_connect_message serv_msg;
  int sock[2] = {-1, -1};

  // Create a socket pair for the server to notify of audio events.
  rc = socketpair(AF_UNIX, SOCK_STREAM, 0, sock);
  if (rc != 0) {
    rc = -errno;
    syslog(LOG_WARNING, "cras_client: socketpair: %s", cras_strerror(-rc));
    goto fail;
  }

  cras_fill_connect_message(
      &serv_msg, stream->config->direction, stream->id,
      stream->config->stream_type, stream->config->client_type,
      stream->config->buffer_frames, stream->config->cb_threshold,
      stream->flags, stream->config->effects, stream->config->format, dev_idx);

  rc = cras_send_with_fds(client->server_fd, &serv_msg, sizeof(serv_msg),
                          &sock[1], 1);
  if (rc != sizeof(serv_msg)) {
    rc = EIO;
    syslog(LOG_WARNING, "cras_client: add_stream: Send server message failed.");
    goto fail;
  }

  stream->aud_fd = sock[0];
  close(sock[1]);
  return 0;

fail:
  if (sock[0] != -1) {
    close(sock[0]);
  }
  if (sock[1] != -1) {
    close(sock[1]);
  }
  return rc;
}

/* Adds a stream to a running client.  Checks to make sure that the client is
 * attached, waits if it isn't.  The stream is prepared on the  main thread and
 * passed here. */
static int client_thread_add_stream(struct cras_client* client,
                                    struct client_stream* stream,
                                    cras_stream_id_t* stream_id_out,
                                    uint32_t dev_idx) {
  int rc;
  cras_stream_id_t new_id;
  struct client_stream* out;

  if ((stream->flags & HOTWORD_STREAM) == HOTWORD_STREAM) {
    int hotword_idx;
    hotword_idx = cras_client_get_first_dev_type_idx(
        client, CRAS_NODE_TYPE_HOTWORD, CRAS_STREAM_INPUT);

    // Find the hotword device index.
    if (dev_idx == NO_DEVICE) {
      if (hotword_idx < 0) {
        syslog(LOG_WARNING, "cras_client: add_stream: No hotword dev");
        return hotword_idx;
      } else {
        dev_idx = (uint32_t)hotword_idx;
      }
    }
    /* A known Use case for client to pin hotword stream on a not
     * hotword device is to use internal mic for Assistant to work
     * on board without usable DSP hotwording. We assume there will
     * be only one hotword device exists. */
    else if (dev_idx != (uint32_t)hotword_idx) {
      /* Unmask the flag to fallback to normal pinned stream
       * on specified device. */
      stream->flags &= ~HOTWORD_STREAM;
    }
  }

  // Find an available stream id.
  do {
    new_id = cras_get_stream_id(client->id, client->next_stream_id);
    client->next_stream_id++;
    DL_SEARCH_SCALAR(client->streams, out, id, new_id);
  } while (out != NULL);

  stream->id = new_id;
  *stream_id_out = new_id;
  stream->client = client;

  // Start the audio thread.
  rc = start_aud_thread(stream);
  if (rc != 0) {
    return rc;
  }

  // Start the thread associated with this stream.
  // send a message to the server asking that the stream be started.
  rc = send_connect_message(client, stream, dev_idx);
  if (rc != 0) {
    stop_aud_thread(stream, 1);
    return rc;
  }

  // Add the stream to the linked list
  DL_APPEND(client->streams, stream);

  return 0;
}

/* Removes a stream from a running client from within the running client's
 * context. */
static int client_thread_rm_stream(struct cras_client* client,
                                   cras_stream_id_t stream_id) {
  struct cras_disconnect_stream_message msg;
  struct client_stream* stream = stream_from_id(client, stream_id);
  int rc;

  if (stream == NULL) {
    return 0;
  }

  // Tell server to remove.
  if (client->server_fd_state == CRAS_SOCKET_STATE_CONNECTED) {
    cras_fill_disconnect_stream_message(&msg, stream_id);
    rc = write(client->server_fd, &msg, sizeof(msg));
    if (rc < 0) {
      syslog(LOG_WARNING, "cras_client: error removing stream from server\n");
    }
  }

  // And shut down locally.
  stop_aud_thread(stream, 1);

  free_shm(stream);

  DL_DELETE(client->streams, stream);
  if (stream->aud_fd >= 0) {
    close(stream->aud_fd);
  }

  free(stream->config);
  free(stream);

  return 0;
}

static int client_thread_set_aec_ref(struct cras_client* client,
                                     cras_stream_id_t stream_id,
                                     uint32_t dev_idx) {
  struct cras_set_aec_ref_message msg;
  struct client_stream* stream = stream_from_id(client, stream_id);
  int rc;

  if (stream == NULL) {
    return 0;
  }

  if (client->server_fd_state == CRAS_SOCKET_STATE_CONNECTED) {
    cras_fill_set_aec_ref_message(&msg, stream_id, dev_idx);
    rc = write(client->server_fd, &msg, sizeof(msg));
    if (rc < 0) {
      syslog(LOG_WARNING, "cras_client: error setting aec ref\n");
    }
  }
  return 0;
}

// Sets the volume scaling factor for a playback or capture stream.
static int client_thread_set_stream_volume(struct cras_client* client,
                                           cras_stream_id_t stream_id,
                                           float volume_scaler) {
  struct client_stream* stream;

  stream = stream_from_id(client, stream_id);
  if (stream == NULL || volume_scaler > 1.0 || volume_scaler < 0.0) {
    return -EINVAL;
  }

  stream->volume_scaler = volume_scaler;
  if (stream->shm) {
    cras_shm_set_volume_scaler(stream->shm, volume_scaler);
  }

  return 0;
}

// Attach to the shm region containing the audio thread log.
static void attach_atlog_shm(struct cras_client* client, int fd) {
  client->atlog_ro = (struct audio_thread_event_log*)mmap(
      NULL, sizeof(*client->atlog_ro), PROT_READ, MAP_SHARED, fd, 0);
  close(fd);
}

// Attach to the shm region containing the server state.
static int client_attach_shm(struct cras_client* client, int shm_fd) {
  int lock_rc;
  int rc;

  lock_rc = server_state_wrlock(client);
  if (client->server_state) {
    rc = -EBUSY;
    goto error;
  }

  client->server_state = (struct cras_server_state*)mmap(
      NULL, sizeof(*client->server_state), PROT_READ, MAP_SHARED, shm_fd, 0);
  rc = -errno;
  close(shm_fd);
  if (client->server_state == (struct cras_server_state*)-1) {
    syslog(LOG_WARNING, "cras_client: mmap failed to map shm for client: %s",
           cras_strerror(-rc));
    goto error;
  }

  if (client->server_state->state_version != CRAS_SERVER_STATE_VERSION) {
    munmap((void*)client->server_state, sizeof(*client->server_state));
    client->server_state = NULL;
    rc = -EINVAL;
    syslog(LOG_WARNING, "cras_client: Unknown server_state version.");
  } else {
    rc = 0;
  }

error:
  server_state_unlock(client, lock_rc);
  return rc;
}

static void cras_client_get_hotword_models_ready(struct cras_client* client,
                                                 const char* hotword_models) {
  if (!client->get_hotword_models_cb) {
    return;
  }
  client->get_hotword_models_cb(client, hotword_models);
  client->get_hotword_models_cb = NULL;
}

static void request_floop_ready(struct cras_client* client,
                                int32_t dev_idx,
                                uint64_t tag) {
  struct floop_request* req;
  pthread_mutex_lock(&client->floop_request_list_mu);

  DL_FOREACH (client->floop_request_list, req) {
    if (req == (struct floop_request*)(uintptr_t)tag) {
      break;
    }
  }

  if (req == NULL) {
    goto cleanup;
  }

  pthread_mutex_lock(&req->mu);
  req->response = dev_idx;
  req->fullfilled = true;
  pthread_cond_broadcast(&req->cond);
  pthread_mutex_unlock(&req->mu);

cleanup:
  pthread_mutex_unlock(&client->floop_request_list_mu);
  return;
}

// Handles messages from the cras server.
static int handle_message_from_server(struct cras_client* client) {
  uint8_t buf[CRAS_CLIENT_MAX_MSG_SIZE];
  struct cras_client_message* msg;
  int rc = 0;
  int nread;
  int server_fds[2];
  unsigned int num_fds = 2;

  msg = (struct cras_client_message*)buf;
  nread = cras_recv_with_fds(client->server_fd, buf, sizeof(buf), server_fds,
                             &num_fds);
  if (nread < (int)sizeof(msg->length) || (int)msg->length != nread) {
    return -EIO;
  }

  switch (msg->id) {
    case CRAS_CLIENT_CONNECTED: {
      struct cras_client_connected* cmsg = (struct cras_client_connected*)msg;
      if (num_fds != 1) {
        return -EINVAL;
      }
      rc = client_attach_shm(client, server_fds[0]);
      if (rc) {
        return rc;
      }
      client->id = cmsg->client_id;

      break;
    }
    case CRAS_CLIENT_STREAM_CONNECTED: {
      struct cras_client_stream_connected* cmsg =
          (struct cras_client_stream_connected*)msg;
      struct client_stream* stream = stream_from_id(client, cmsg->stream_id);
      if (stream == NULL) {
        if (num_fds != 2) {
          syslog(LOG_WARNING,
                 "cras_client: Error receiving "
                 "stream 0x%x connected message",
                 cmsg->stream_id);
          return -EINVAL;
        }

        /*
         * Usually, the fds should be closed in stream_connected
         * callback. However, sometimes a stream is removed
         * before it is connected.
         */
        close(server_fds[0]);
        close(server_fds[1]);
        break;
      }
      rc = stream_connected(stream, cmsg, server_fds, num_fds);
      if (rc < 0) {
        stream->config->err_cb(stream->client, stream->id, rc,
                               stream->config->user_data);
      }
      break;
    }
    case CRAS_CLIENT_AUDIO_DEBUG_INFO_READY:
      if (client->debug_info_callback) {
        client->debug_info_callback(client);
      }
      client->debug_info_callback = NULL;
      break;
    case CRAS_CLIENT_ATLOG_FD_READY:
      if (num_fds != 1 || server_fds[0] < 0) {
        return -EINVAL;
      }
      attach_atlog_shm(client, server_fds[0]);
      if (client->atlog_access_callback) {
        client->atlog_access_callback(client);
      }
      client->atlog_access_callback = NULL;
      break;
    case CRAS_CLIENT_GET_HOTWORD_MODELS_READY: {
      struct cras_client_get_hotword_models_ready* cmsg =
          (struct cras_client_get_hotword_models_ready*)msg;
      cras_client_get_hotword_models_ready(client,
                                           (const char*)cmsg->hotword_models);
      break;
    }
    case CRAS_CLIENT_REQUEST_FLOOP_READY: {
      struct cras_client_request_floop_ready* cmsg =
          (struct cras_client_request_floop_ready*)msg;
      request_floop_ready(client, cmsg->dev_idx, cmsg->tag);
      break;
    }
    case CRAS_CLIENT_OUTPUT_VOLUME_CHANGED: {
      struct cras_client_volume_changed* cmsg =
          (struct cras_client_volume_changed*)msg;
      if (client->observer_ops.output_volume_changed) {
        client->observer_ops.output_volume_changed(client->observer_context,
                                                   cmsg->volume);
      }
      break;
    }
    case CRAS_CLIENT_OUTPUT_MUTE_CHANGED: {
      struct cras_client_mute_changed* cmsg =
          (struct cras_client_mute_changed*)msg;
      if (client->observer_ops.output_mute_changed) {
        client->observer_ops.output_mute_changed(client->observer_context,
                                                 cmsg->muted, cmsg->user_muted,
                                                 cmsg->mute_locked);
      }
      break;
    }
    case CRAS_CLIENT_CAPTURE_GAIN_CHANGED: {
      struct cras_client_volume_changed* cmsg =
          (struct cras_client_volume_changed*)msg;
      if (client->observer_ops.capture_gain_changed) {
        client->observer_ops.capture_gain_changed(client->observer_context,
                                                  cmsg->volume);
      }
      break;
    }
    case CRAS_CLIENT_CAPTURE_MUTE_CHANGED: {
      struct cras_client_mute_changed* cmsg =
          (struct cras_client_mute_changed*)msg;
      if (client->observer_ops.capture_mute_changed) {
        client->observer_ops.capture_mute_changed(
            client->observer_context, cmsg->muted, cmsg->mute_locked);
      }
      break;
    }
    case CRAS_CLIENT_NODES_CHANGED: {
      if (client->observer_ops.nodes_changed) {
        client->observer_ops.nodes_changed(client->observer_context);
      }
      break;
    }
    case CRAS_CLIENT_ACTIVE_NODE_CHANGED: {
      struct cras_client_active_node_changed* cmsg =
          (struct cras_client_active_node_changed*)msg;
      enum CRAS_STREAM_DIRECTION direction =
          (enum CRAS_STREAM_DIRECTION)cmsg->direction;
      if (client->observer_ops.active_node_changed) {
        client->observer_ops.active_node_changed(client->observer_context,
                                                 direction, cmsg->node_id);
      }
      break;
    }
    case CRAS_CLIENT_OUTPUT_NODE_VOLUME_CHANGED: {
      struct cras_client_node_value_changed* cmsg =
          (struct cras_client_node_value_changed*)msg;
      if (client->observer_ops.output_node_volume_changed) {
        client->observer_ops.output_node_volume_changed(
            client->observer_context, cmsg->node_id, cmsg->value);
      }
      break;
    }
    case CRAS_CLIENT_NODE_LEFT_RIGHT_SWAPPED_CHANGED: {
      struct cras_client_node_value_changed* cmsg =
          (struct cras_client_node_value_changed*)msg;
      if (client->observer_ops.node_left_right_swapped_changed) {
        client->observer_ops.node_left_right_swapped_changed(
            client->observer_context, cmsg->node_id, cmsg->value);
      }
      break;
    }
    case CRAS_CLIENT_INPUT_NODE_GAIN_CHANGED: {
      struct cras_client_node_value_changed* cmsg =
          (struct cras_client_node_value_changed*)msg;
      if (client->observer_ops.input_node_gain_changed) {
        client->observer_ops.input_node_gain_changed(
            client->observer_context, cmsg->node_id, cmsg->value);
      }
      break;
    }
    case CRAS_CLIENT_NUM_ACTIVE_STREAMS_CHANGED: {
      struct cras_client_num_active_streams_changed* cmsg =
          (struct cras_client_num_active_streams_changed*)msg;
      enum CRAS_STREAM_DIRECTION direction =
          (enum CRAS_STREAM_DIRECTION)cmsg->direction;
      if (client->observer_ops.num_active_streams_changed) {
        client->observer_ops.num_active_streams_changed(
            client->observer_context, direction, cmsg->num_active_streams);
      }
      break;
    }
    default:
      break;
  }

  return 0;
}

// Handles messages from streams to this client.
static int handle_stream_message(struct cras_client* client, int poll_revents) {
  struct stream_msg msg;
  int rc;

  if ((poll_revents & POLLIN) == 0) {
    return 0;
  }

  rc = read(client->stream_fds[0], &msg, sizeof(msg));
  if (rc < 0) {
    syslog(LOG_WARNING, "cras_client: Stream read failed %d\n", errno);
  }
  /* The only reason a stream sends a message is if it needs to be
   * removed. An error on read would mean the same thing so regardless of
   * what gets us here, just remove the stream */
  client_thread_rm_stream(client, msg.stream_id);
  return 0;
}

// Handles messages from users to this client.
static int handle_command_message(struct cras_client* client,
                                  int poll_revents) {
  uint8_t buf[MAX_CMD_MSG_LEN];
  struct command_msg* msg = (struct command_msg*)buf;
  int rc, to_read;

  if ((poll_revents & POLLIN) == 0) {
    return 0;
  }

  rc = read(client->command_fds[0], buf, sizeof(msg->len));
  if (rc != sizeof(msg->len) || msg->len > MAX_CMD_MSG_LEN) {
    rc = -EIO;
    goto cmd_msg_complete;
  }
  to_read = msg->len - rc;
  rc = read(client->command_fds[0], &buf[0] + rc, to_read);
  if (rc != to_read) {
    rc = -EIO;
    goto cmd_msg_complete;
  }

  switch (msg->msg_id) {
    case CLIENT_STOP: {
      struct client_stream* s;

      // Stop all playing streams
      DL_FOREACH (client->streams, s) {
        client_thread_rm_stream(client, s->id);
      }

      // And stop this client
      client->thread.state = CRAS_THREAD_STOP;
      rc = 0;
      break;
    }
    case CLIENT_ADD_STREAM: {
      struct add_stream_command_message* add_msg =
          (struct add_stream_command_message*)msg;
      rc = client_thread_add_stream(client, add_msg->stream,
                                    add_msg->stream_id_out, add_msg->dev_idx);
      break;
    }
    case CLIENT_REMOVE_STREAM:
      rc = client_thread_rm_stream(client, msg->stream_id);
      break;
    case CLIENT_SET_STREAM_VOLUME_SCALER: {
      struct set_stream_volume_command_message* vol_msg =
          (struct set_stream_volume_command_message*)msg;
      rc = client_thread_set_stream_volume(client, vol_msg->header.stream_id,
                                           vol_msg->volume_scaler);
      break;
    }
    case CLIENT_SET_AEC_REF: {
      struct set_aec_ref_command_message* set_aec_ref =
          (struct set_aec_ref_command_message*)msg;
      rc = client_thread_set_aec_ref(client, msg->stream_id,
                                     set_aec_ref->dev_idx);
      break;
    }
    case CLIENT_SERVER_CONNECT:
      rc = connect_to_server_wait(client, false);
      break;
    case CLIENT_SERVER_CONNECT_ASYNC:
      rc = server_connect(client);
      break;
    default:
      assert(0);
      break;
  }

cmd_msg_complete:
  // Wake the waiting main thread with the result of the command.
  if (write(client->command_reply_fds[1], &rc, sizeof(rc)) != sizeof(rc)) {
    return -EIO;
  }
  return rc;
}

/*  This thread handles non audio sample communication with the audio server.
 *  The client program will call fucntions below to send messages to this thread
 *  to add or remove streams or change parameters.
 */
static void* client_thread(void* arg) {
  struct cras_client* client = (struct cras_client*)arg;
  struct pollfd pollfds[4];
  int (*cbs[4])(struct cras_client* client, int poll_revents);
  unsigned int num_pollfds, i;
  int rc;

  if (arg == NULL) {
    return (void*)-EINVAL;
  }

  while (thread_is_running(&client->thread)) {
    num_pollfds = 0;

    rc = cras_file_wait_get_fd(client->sock_file_wait);
    if (rc >= 0) {
      cbs[num_pollfds] = sock_file_wait_dispatch;
      pollfds[num_pollfds].fd = rc;
      pollfds[num_pollfds].events = POLLIN;
      pollfds[num_pollfds].revents = 0;
      num_pollfds++;
    } else {
      syslog(LOG_WARNING, "file wait fd: %d", rc);
    }
    if (client->server_fd >= 0) {
      cbs[num_pollfds] = server_fd_dispatch;
      server_fill_pollfd(client, &(pollfds[num_pollfds]));
      num_pollfds++;
    }
    if (client->command_fds[0] >= 0) {
      cbs[num_pollfds] = handle_command_message;
      pollfds[num_pollfds].fd = client->command_fds[0];
      pollfds[num_pollfds].events = POLLIN;
      pollfds[num_pollfds].revents = 0;
      num_pollfds++;
    }
    if (client->stream_fds[0] >= 0) {
      cbs[num_pollfds] = handle_stream_message;
      pollfds[num_pollfds].fd = client->stream_fds[0];
      pollfds[num_pollfds].events = POLLIN;
      pollfds[num_pollfds].revents = 0;
      num_pollfds++;
    }

    rc = poll(pollfds, num_pollfds, -1);
    if (rc <= 0) {
      continue;
    }

    for (i = 0; i < num_pollfds; i++) {
      /* Only do one at a time, since some messages may
       * result in change to other fds. */
      if (pollfds[i].revents) {
        cbs[i](client, pollfds[i].revents);
        break;
      }
    }
  }

  // close the command reply pipe.
  close(client->command_reply_fds[1]);
  client->command_reply_fds[1] = -1;

  return NULL;
}

/* Sends a message to the client thread to complete an action requested by the
 * user.  Then waits for the action to complete and returns the result. */
static int send_command_message(struct cras_client* client,
                                struct command_msg* msg) {
  int rc, cmd_res;
  if (client == NULL || !thread_is_running(&client->thread)) {
    return -EINVAL;
  }

  rc = write(client->command_fds[1], msg, msg->len);
  if (rc != (int)msg->len) {
    return -EPIPE;
  }

  // Wait for command to complete.
  rc = read(client->command_reply_fds[0], &cmd_res, sizeof(cmd_res));
  if (rc != sizeof(cmd_res)) {
    return -EPIPE;
  }
  return cmd_res;
}

// Send a simple message to the client thread that holds no data.
static int send_simple_cmd_msg(struct cras_client* client,
                               cras_stream_id_t stream_id,
                               unsigned msg_id) {
  struct command_msg msg;

  msg.len = sizeof(msg);
  msg.stream_id = stream_id;
  msg.msg_id = msg_id;

  return send_command_message(client, &msg);
}

// Sends the set volume message to the client thread.
static int send_stream_volume_command_msg(struct cras_client* client,
                                          cras_stream_id_t stream_id,
                                          float volume_scaler) {
  struct set_stream_volume_command_message msg;

  msg.header.len = sizeof(msg);
  msg.header.stream_id = stream_id;
  msg.header.msg_id = CLIENT_SET_STREAM_VOLUME_SCALER;
  msg.volume_scaler = volume_scaler;

  return send_command_message(client, &msg.header);
}

// Sends a message to set AEC ref device id for given stream.
static int send_set_aec_ref_command_msg(struct cras_client* client,
                                        cras_stream_id_t stream_id,
                                        uint32_t dev_idx) {
  struct set_aec_ref_command_message msg;

  msg.header.len = sizeof(msg);
  msg.header.msg_id = CLIENT_SET_AEC_REF;
  msg.header.stream_id = stream_id;
  msg.dev_idx = dev_idx;
  return send_command_message(client, &msg.header);
}

// Sends a message back to the client and returns the error code.
static int write_message_to_server(struct cras_client* client,
                                   const struct cras_server_message* msg) {
  ssize_t write_rc = -EPIPE;

  if (client->server_fd_state == CRAS_SOCKET_STATE_CONNECTED ||
      client->server_fd_state == CRAS_SOCKET_STATE_FIRST_MESSAGE) {
    write_rc = write(client->server_fd, msg, msg->length);
    if (write_rc < 0) {
      write_rc = -errno;
    }
  }

  if (write_rc != (ssize_t)msg->length &&
      client->server_fd_state != CRAS_SOCKET_STATE_FIRST_MESSAGE) {
    return -EPIPE;
  }

  if (write_rc < 0) {
    return write_rc;
  } else if (write_rc != (ssize_t)msg->length) {
    return -EIO;
  } else {
    return 0;
  }
}

// Fills server socket file to connect by client's connection type.
static int fill_socket_file(struct cras_client* client,
                            enum CRAS_CONNECTION_TYPE conn_type) {
  int rc;

  client->sock_file =
      (const char*)calloc(CRAS_MAX_SOCKET_PATH_SIZE, sizeof(char));
  if (client->sock_file == NULL) {
    return -ENOMEM;
  }

  rc = cras_fill_socket_path(conn_type, (char*)client->sock_file);
  if (rc < 0) {
    free((void*)client->sock_file);
    return rc;
  }
  return 0;
}

/*
 * Exported Client Interface
 */

int cras_client_create_with_type(struct cras_client** client,
                                 enum CRAS_CONNECTION_TYPE conn_type) {
  int rc;
  struct client_int* client_int;
  pthread_condattr_t cond_attr;

  if (!cras_validate_connection_type(conn_type)) {
    syslog(LOG_WARNING, "Input connection type is not supported.\n");
    return -EINVAL;
  }

  // Ignore SIGPIPE while using this API.
  signal(SIGPIPE, SIG_IGN);

  client_int = (struct client_int*)calloc(1, sizeof(*client_int));
  if (!client_int) {
    return -ENOMEM;
  }
  *client = &client_int->client;
  (*client)->server_fd = -1;
  (*client)->id = -1;

  rc = pthread_rwlock_init(&client_int->server_state_rwlock, NULL);
  if (rc != 0) {
    syslog(LOG_WARNING, "cras_client: Could not init state rwlock.");
    rc = -rc;
    goto free_client;
  }

  rc = pthread_mutex_init(&(*client)->stream_start_lock, NULL);
  if (rc != 0) {
    syslog(LOG_WARNING, "cras_client: Could not init start lock.");
    rc = -rc;
    goto free_rwlock;
  }

  pthread_condattr_init(&cond_attr);
  pthread_condattr_setclock(&cond_attr, CLOCK_MONOTONIC);
  rc = pthread_cond_init(&(*client)->stream_start_cond, &cond_attr);
  pthread_condattr_destroy(&cond_attr);
  if (rc != 0) {
    syslog(LOG_WARNING, "cras_client: Could not init start cond.");
    rc = -rc;
    goto free_lock;
  }

  (*client)->server_event_fd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
  if ((*client)->server_event_fd < 0) {
    syslog(LOG_WARNING, "cras_client: Could not setup server eventfd.");
    rc = -errno;
    goto free_cond;
  }

  rc = fill_socket_file((*client), conn_type);
  if (rc < 0) {
    goto free_server_event_fd;
  }

  rc = cras_file_wait_create((*client)->sock_file, CRAS_FILE_WAIT_FLAG_NONE,
                             sock_file_wait_callback, *client,
                             &(*client)->sock_file_wait);
  if (rc < 0 && rc != -ENOENT) {
    syslog(LOG_WARNING, "cras_client: Could not setup watch for '%s': %s",
           (*client)->sock_file, cras_strerror(-rc));
    goto free_error;
  }
  (*client)->sock_file_exists = (rc == 0);

  /* Pipes used by the main thread and the client thread to send commands
   * and replies. */
  rc = pipe((*client)->command_fds);
  if (rc < 0) {
    goto free_error;
  }
  /* Pipe used to communicate between the client thread and the audio
   * thread. */
  rc = pipe((*client)->stream_fds);
  if (rc < 0) {
    close((*client)->command_fds[0]);
    close((*client)->command_fds[1]);
    goto free_error;
  }
  (*client)->command_reply_fds[0] = -1;
  (*client)->command_reply_fds[1] = -1;

  return 0;
free_error:
  cras_file_wait_destroy((*client)->sock_file_wait);
  free((void*)(*client)->sock_file);
free_server_event_fd:
  if ((*client)->server_event_fd >= 0) {
    close((*client)->server_event_fd);
  }
free_cond:
  pthread_cond_destroy(&(*client)->stream_start_cond);
free_lock:
  pthread_mutex_destroy(&(*client)->stream_start_lock);
free_rwlock:
  pthread_rwlock_destroy(&client_int->server_state_rwlock);
free_client:
  *client = NULL;
  free(client_int);
  return rc;
}

int cras_client_create(struct cras_client** client) {
  return cras_client_create_with_type(client, CRAS_CONTROL);
}

void cras_client_destroy(struct cras_client* client) {
  struct client_int* client_int;
  if (client == NULL) {
    return;
  }
  client_int = to_client_int(client);
  client->server_connection_cb = NULL;
  cras_client_stop(client);
  server_disconnect(client);
  close(client->server_event_fd);
  close(client->command_fds[0]);
  close(client->command_fds[1]);
  close(client->stream_fds[0]);
  close(client->stream_fds[1]);
  cras_file_wait_destroy(client->sock_file_wait);
  pthread_rwlock_destroy(&client_int->server_state_rwlock);
  free((void*)client->sock_file);
  free(client_int);
}

int cras_client_connect(struct cras_client* client) {
  return connect_to_server(client, NULL, true);
}

int cras_client_connect_timeout(struct cras_client* client,
                                unsigned int timeout_ms) {
  return connect_to_server_wait_retry(client, timeout_ms, true);
}

int cras_client_connected_wait(struct cras_client* client) {
  return send_simple_cmd_msg(client, 0, CLIENT_SERVER_CONNECT);
}

int cras_client_connect_async(struct cras_client* client) {
  return send_simple_cmd_msg(client, 0, CLIENT_SERVER_CONNECT_ASYNC);
}

int cras_client_set_client_type(struct cras_client* client,
                                enum CRAS_CLIENT_TYPE client_type) {
  if (client == NULL) {
    return -EINVAL;
  }
  client->client_type = client_type;
  return 0;
}

struct cras_stream_params* cras_client_stream_params_create(
    enum CRAS_STREAM_DIRECTION direction,
    size_t buffer_frames,
    size_t cb_threshold,
    size_t unused,
    enum CRAS_STREAM_TYPE stream_type,
    uint32_t flags,
    void* user_data,
    cras_playback_cb_t aud_cb,
    cras_error_cb_t err_cb,
    struct cras_audio_format* format) {
  struct cras_stream_params* params;

  params = (struct cras_stream_params*)malloc(sizeof(*params));
  if (params == NULL) {
    return NULL;
  }

  params->direction = direction;
  params->buffer_frames = buffer_frames;
  params->cb_threshold = cb_threshold;
  params->effects = 0;
  params->stream_type = stream_type;
  params->client_type = CRAS_CLIENT_TYPE_UNKNOWN;
  params->flags = flags;
  params->user_data = user_data;
  params->aud_cb = aud_cb;
  params->unified_cb = 0;
  params->stream_cb = 0;
  params->err_cb = err_cb;
  memcpy(&(params->format), format, sizeof(*format));
  return params;
}

void cras_client_stream_params_set_client_type(
    struct cras_stream_params* params,
    enum CRAS_CLIENT_TYPE client_type) {
  params->client_type = client_type;
}

void cras_client_stream_params_enable_aec(struct cras_stream_params* params) {
  params->effects |= APM_ECHO_CANCELLATION;
}

void cras_client_stream_params_disable_aec(struct cras_stream_params* params) {
  params->effects &= ~APM_ECHO_CANCELLATION;
}

void cras_client_stream_params_enable_ns(struct cras_stream_params* params) {
  params->effects |= APM_NOISE_SUPRESSION;
}

void cras_client_stream_params_disable_ns(struct cras_stream_params* params) {
  params->effects &= ~APM_NOISE_SUPRESSION;
}

void cras_client_stream_params_enable_agc(struct cras_stream_params* params) {
  params->effects |= APM_GAIN_CONTROL;
}

void cras_client_stream_params_disable_agc(struct cras_stream_params* params) {
  params->effects &= ~APM_GAIN_CONTROL;
}

void cras_client_stream_params_enable_vad(struct cras_stream_params* params) {
  params->effects |= APM_VOICE_DETECTION;
}

void cras_client_stream_params_disable_vad(struct cras_stream_params* params) {
  params->effects &= ~APM_VOICE_DETECTION;
}

void cras_client_stream_params_allow_aec_on_dsp(
    struct cras_stream_params* params) {
  params->effects |= DSP_ECHO_CANCELLATION_ALLOWED;
}

void cras_client_stream_params_disallow_aec_on_dsp(
    struct cras_stream_params* params) {
  params->effects &= ~DSP_ECHO_CANCELLATION_ALLOWED;
}

void cras_client_stream_params_allow_ns_on_dsp(
    struct cras_stream_params* param) {
  param->effects |= DSP_NOISE_SUPPRESSION_ALLOWED;
}

void cras_client_stream_params_disallow_ns_on_dsp(
    struct cras_stream_params* params) {
  params->effects &= ~DSP_NOISE_SUPPRESSION_ALLOWED;
}
void cras_client_stream_params_allow_agc_on_dsp(
    struct cras_stream_params* params) {
  params->effects |= DSP_GAIN_CONTROL_ALLOWED;
}

void cras_client_stream_params_disallow_agc_on_dsp(
    struct cras_stream_params* params) {
  params->effects &= ~DSP_GAIN_CONTROL_ALLOWED;
}

void cras_client_stream_params_enable_ignore_ui_gains(
    struct cras_stream_params* params) {
  params->effects |= IGNORE_UI_GAINS;
}

void cras_client_stream_params_disable_ignore_ui_gains(
    struct cras_stream_params* params) {
  params->effects &= ~IGNORE_UI_GAINS;
}

struct cras_stream_params* cras_client_unified_params_create(
    enum CRAS_STREAM_DIRECTION direction,
    unsigned int block_size,
    enum CRAS_STREAM_TYPE stream_type,
    uint32_t flags,
    void* user_data,
    cras_unified_cb_t unified_cb,
    cras_error_cb_t err_cb,
    struct cras_audio_format* format) {
  struct cras_stream_params* params;

  params = (struct cras_stream_params*)malloc(sizeof(*params));
  if (params == NULL) {
    return NULL;
  }

  params->direction = direction;
  params->buffer_frames = block_size * 2;
  params->cb_threshold = block_size;
  params->stream_type = stream_type;
  params->client_type = CRAS_CLIENT_TYPE_UNKNOWN;
  params->flags = flags;
  params->effects = 0;
  params->user_data = user_data;
  params->aud_cb = 0;
  params->unified_cb = unified_cb;
  params->stream_cb = 0;
  params->err_cb = err_cb;
  memcpy(&(params->format), format, sizeof(*format));

  return params;
}

void cras_client_stream_params_destroy(struct cras_stream_params* params) {
  free(params);
}

static inline int cras_client_send_add_stream_command_message(
    struct cras_client* client,
    uint32_t dev_idx,
    cras_stream_id_t* stream_id_out,
    struct cras_stream_params* config) {
  struct add_stream_command_message cmd_msg;
  struct client_stream* stream;
  int rc = 0;

  if (client == NULL || config == NULL || stream_id_out == NULL) {
    return -EINVAL;
  }

  if (config->stream_cb == NULL && config->aud_cb == NULL &&
      config->unified_cb == NULL) {
    return -EINVAL;
  }

  if (config->err_cb == NULL) {
    return -EINVAL;
  }

  stream = (struct client_stream*)calloc(1, sizeof(*stream));
  if (stream == NULL) {
    rc = -ENOMEM;
    goto add_failed;
  }
  stream->config =
      (struct cras_stream_params*)malloc(sizeof(*(stream->config)));
  if (stream->config == NULL) {
    rc = -ENOMEM;
    goto add_failed;
  }
  memcpy(stream->config, config, sizeof(*config));

  /* To be consistent with existing behavior, temporarily override the client
   * type if one is specified in the stream params. */
  if (stream->config->client_type == CRAS_CLIENT_TYPE_UNKNOWN) {
    stream->config->client_type = client->client_type;
  }

  stream->aud_fd = -1;
  stream->wake_fds[0] = -1;
  stream->wake_fds[1] = -1;
  stream->direction = config->direction;
  stream->flags = config->flags;

  /* Caller might not set this volume scaler after stream created,
   * so always initialize it to 1.0f */
  stream->volume_scaler = 1.0f;

  cmd_msg.header.len = sizeof(cmd_msg);
  cmd_msg.header.msg_id = CLIENT_ADD_STREAM;
  cmd_msg.header.stream_id = stream->id;
  cmd_msg.stream = stream;
  cmd_msg.stream_id_out = stream_id_out;
  cmd_msg.dev_idx = dev_idx;
  rc = send_command_message(client, &cmd_msg.header);
  if (rc < 0) {
    syslog(LOG_WARNING, "cras_client: adding stream failed in thread %d", rc);
    goto add_failed;
  }

  return 0;

add_failed:
  if (stream) {
    if (stream->config) {
      free(stream->config);
    }
    free(stream);
  }
  return rc;
}

int cras_client_add_stream(struct cras_client* client,
                           cras_stream_id_t* stream_id_out,
                           struct cras_stream_params* config) {
  return cras_client_send_add_stream_command_message(client, NO_DEVICE,
                                                     stream_id_out, config);
}

int cras_client_add_pinned_stream(struct cras_client* client,
                                  uint32_t dev_idx,
                                  cras_stream_id_t* stream_id_out,
                                  struct cras_stream_params* config) {
  return cras_client_send_add_stream_command_message(client, dev_idx,
                                                     stream_id_out, config);
}

int cras_client_rm_stream(struct cras_client* client,
                          cras_stream_id_t stream_id) {
  if (client == NULL) {
    return -EINVAL;
  }

  return send_simple_cmd_msg(client, stream_id, CLIENT_REMOVE_STREAM);
}

int cras_client_set_stream_volume(struct cras_client* client,
                                  cras_stream_id_t stream_id,
                                  float volume_scaler) {
  if (client == NULL) {
    return -EINVAL;
  }

  return send_stream_volume_command_msg(client, stream_id, volume_scaler);
}

int cras_client_set_aec_ref(struct cras_client* client,
                            cras_stream_id_t stream_id,
                            uint32_t dev_idx) {
  if (client == NULL) {
    return -EINVAL;
  }

  return send_set_aec_ref_command_msg(client, stream_id, dev_idx);
}

int cras_client_set_system_volume(struct cras_client* client, size_t volume) {
  struct cras_set_system_volume msg;

  if (client == NULL) {
    return -EINVAL;
  }

  cras_fill_set_system_volume(&msg, volume);
  return write_message_to_server(client, &msg.header);
}

int cras_client_set_system_mute(struct cras_client* client, int mute) {
  struct cras_set_system_mute msg;

  if (client == NULL) {
    return -EINVAL;
  }

  cras_fill_set_system_mute(&msg, mute);
  return write_message_to_server(client, &msg.header);
}

int cras_client_set_user_mute(struct cras_client* client, int mute) {
  struct cras_set_system_mute msg;

  if (client == NULL) {
    return -EINVAL;
  }

  cras_fill_set_user_mute(&msg, mute);
  return write_message_to_server(client, &msg.header);
}

int cras_client_set_system_mute_locked(struct cras_client* client, int locked) {
  struct cras_set_system_mute msg;

  if (client == NULL) {
    return -EINVAL;
  }

  cras_fill_set_system_mute_locked(&msg, locked);
  return write_message_to_server(client, &msg.header);
}

int cras_client_set_system_capture_mute(struct cras_client* client, int mute) {
  struct cras_set_system_mute msg;

  if (client == NULL) {
    return -EINVAL;
  }

  cras_fill_set_system_capture_mute(&msg, mute);
  return write_message_to_server(client, &msg.header);
}

int cras_client_set_system_capture_mute_locked(struct cras_client* client,
                                               int locked) {
  struct cras_set_system_mute msg;

  if (client == NULL) {
    return -EINVAL;
  }

  cras_fill_set_system_capture_mute_locked(&msg, locked);
  return write_message_to_server(client, &msg.header);
}

size_t cras_client_get_system_volume(const struct cras_client* client) {
  size_t volume;
  int lock_rc;

  lock_rc = server_state_rdlock(client);
  if (lock_rc) {
    return 0;
  }

  volume = client->server_state->volume;
  server_state_unlock(client, lock_rc);
  return volume;
}

long cras_client_get_system_capture_gain(const struct cras_client* client) {
  long gain;
  int lock_rc;

  lock_rc = server_state_rdlock(client);
  if (lock_rc) {
    return 0;
  }

  gain = client->server_state->capture_gain;
  server_state_unlock(client, lock_rc);
  return gain;
}

int cras_client_get_system_muted(const struct cras_client* client) {
  int muted;
  int lock_rc;

  lock_rc = server_state_rdlock(client);
  if (lock_rc) {
    return 0;
  }

  muted = client->server_state->mute;
  server_state_unlock(client, lock_rc);
  return muted;
}

int cras_client_get_user_muted(const struct cras_client* client) {
  int muted;
  int lock_rc;

  lock_rc = server_state_rdlock(client);
  if (lock_rc) {
    return 0;
  }

  muted = client->server_state->user_mute;
  server_state_unlock(client, lock_rc);
  return muted;
}

int cras_client_get_system_capture_muted(const struct cras_client* client) {
  int muted;
  int lock_rc;

  lock_rc = server_state_rdlock(client);
  if (lock_rc) {
    return 0;
  }

  muted = client->server_state->capture_mute;
  server_state_unlock(client, lock_rc);
  return muted;
}

long cras_client_get_system_min_volume(const struct cras_client* client) {
  long min_volume;
  int lock_rc;

  lock_rc = server_state_rdlock(client);
  if (lock_rc) {
    return 0;
  }

  min_volume = client->server_state->min_volume_dBFS;
  server_state_unlock(client, lock_rc);
  return min_volume;
}

long cras_client_get_system_max_volume(const struct cras_client* client) {
  long max_volume;
  int lock_rc;

  lock_rc = server_state_rdlock(client);
  if (lock_rc) {
    return 0;
  }

  max_volume = client->server_state->max_volume_dBFS;
  server_state_unlock(client, lock_rc);
  return max_volume;
}

int cras_client_get_default_output_buffer_size(struct cras_client* client) {
  int default_output_buffer_size;
  int lock_rc;

  lock_rc = server_state_rdlock(client);
  if (lock_rc) {
    return -EINVAL;
  }

  default_output_buffer_size = client->server_state->default_output_buffer_size;
  server_state_unlock(client, lock_rc);
  return default_output_buffer_size;
}

const struct audio_debug_info* cras_client_get_audio_debug_info(
    const struct cras_client* client) {
  const struct audio_debug_info* debug_info;
  int lock_rc;

  lock_rc = server_state_rdlock(client);
  if (lock_rc) {
    return 0;
  }

  debug_info = &client->server_state->audio_debug_info;
  server_state_unlock(client, lock_rc);
  return debug_info;
}

const struct main_thread_debug_info* cras_client_get_main_thread_debug_info(
    const struct cras_client* client) {
  const struct main_thread_debug_info* debug_info;
  int lock_rc;

  lock_rc = server_state_rdlock(client);
  if (lock_rc) {
    return 0;
  }

  debug_info = &client->server_state->main_thread_debug_info;
  server_state_unlock(client, lock_rc);
  return debug_info;
}

const struct cras_bt_debug_info* cras_client_get_bt_debug_info(
    const struct cras_client* client) {
  const struct cras_bt_debug_info* debug_info;
  int lock_rc;

  lock_rc = server_state_rdlock(client);
  if (lock_rc) {
    return 0;
  }

  debug_info = &client->server_state->bt_debug_info;
  server_state_unlock(client, lock_rc);
  return debug_info;
}

const struct cras_audio_thread_snapshot_buffer*
cras_client_get_audio_thread_snapshot_buffer(const struct cras_client* client) {
  const struct cras_audio_thread_snapshot_buffer* snapshot_buffer;
  int lock_rc;

  lock_rc = server_state_rdlock(client);
  if (lock_rc) {
    return 0;
  }

  snapshot_buffer = &client->server_state->snapshot_buffer;
  server_state_unlock(client, lock_rc);
  return snapshot_buffer;
}

unsigned cras_client_get_num_active_streams(const struct cras_client* client,
                                            struct timespec* ts) {
  unsigned num_streams, version, i;
  int lock_rc;

  lock_rc = server_state_rdlock(client);
  if (lock_rc) {
    return 0;
  }

read_active_streams_again:
  version = begin_server_state_read(client->server_state);
  num_streams = 0;
  for (i = 0; i < CRAS_NUM_DIRECTIONS; i++) {
    num_streams += client->server_state->num_active_streams[i];
  }
  if (ts) {
    if (num_streams) {
      clock_gettime(CLOCK_MONOTONIC_RAW, ts);
    } else {
      cras_timespec_to_timespec(ts,
                                &client->server_state->last_active_stream_time);
    }
  }
  if (end_server_state_read(client->server_state, version)) {
    goto read_active_streams_again;
  }

  server_state_unlock(client, lock_rc);
  return num_streams;
}

int cras_client_run_thread(struct cras_client* client) {
  int rc;

  if (client == NULL) {
    return -EINVAL;
  }
  if (thread_is_running(&client->thread)) {
    return 0;
  }

  assert(client->command_reply_fds[0] == -1 &&
         client->command_reply_fds[1] == -1);

  if (pipe(client->command_reply_fds) < 0) {
    return -EIO;
  }
  client->thread.state = CRAS_THREAD_RUNNING;
  rc = pthread_create(&client->thread.tid, NULL, client_thread, client);
  if (rc) {
    client->thread.state = CRAS_THREAD_STOP;
    return -rc;
  }

  return 0;
}

int cras_client_stop(struct cras_client* client) {
  if (client == NULL) {
    return -EINVAL;
  }
  if (!thread_is_running(&client->thread)) {
    return 0;
  }

  send_simple_cmd_msg(client, 0, CLIENT_STOP);
  pthread_join(client->thread.tid, NULL);

  /* The other end of the reply pipe is closed by the client thread, just
   * clost the read end here. */
  close(client->command_reply_fds[0]);
  client->command_reply_fds[0] = -1;

  return 0;
}

void cras_client_set_connection_status_cb(
    struct cras_client* client,
    cras_connection_status_cb_t connection_cb,
    void* user_arg) {
  client->server_connection_cb = connection_cb;
  client->server_connection_user_arg = user_arg;
}

void cras_client_set_thread_priority_cb(struct cras_client* client,
                                        cras_thread_priority_cb_t cb) {
  client->thread_priority_cb = cb;
}

int cras_client_get_output_devices(const struct cras_client* client,
                                   struct cras_iodev_info* devs,
                                   struct cras_ionode_info* nodes,
                                   size_t* num_devs,
                                   size_t* num_nodes) {
  const struct cras_server_state* state;
  unsigned avail_devs, avail_nodes, version;
  int lock_rc;

  lock_rc = server_state_rdlock(client);
  if (lock_rc) {
    return -EINVAL;
  }
  state = client->server_state;

read_outputs_again:
  version = begin_server_state_read(state);
  avail_devs = MIN(*num_devs, state->num_output_devs);
  memcpy(devs, state->output_devs, avail_devs * sizeof(*devs));
  avail_nodes = MIN(*num_nodes, state->num_output_nodes);
  memcpy(nodes, state->output_nodes, avail_nodes * sizeof(*nodes));
  if (end_server_state_read(state, version)) {
    goto read_outputs_again;
  }
  server_state_unlock(client, lock_rc);

  *num_devs = avail_devs;
  *num_nodes = avail_nodes;

  return 0;
}

int cras_client_get_input_devices(const struct cras_client* client,
                                  struct cras_iodev_info* devs,
                                  struct cras_ionode_info* nodes,
                                  size_t* num_devs,
                                  size_t* num_nodes) {
  const struct cras_server_state* state;
  unsigned avail_devs, avail_nodes, version;
  int lock_rc;

  lock_rc = server_state_rdlock(client);
  if (!client) {
    return -EINVAL;
  }
  state = client->server_state;

read_inputs_again:
  version = begin_server_state_read(state);
  avail_devs = MIN(*num_devs, state->num_input_devs);
  memcpy(devs, state->input_devs, avail_devs * sizeof(*devs));
  avail_nodes = MIN(*num_nodes, state->num_input_nodes);
  memcpy(nodes, state->input_nodes, avail_nodes * sizeof(*nodes));
  if (end_server_state_read(state, version)) {
    goto read_inputs_again;
  }
  server_state_unlock(client, lock_rc);

  *num_devs = avail_devs;
  *num_nodes = avail_nodes;

  return 0;
}

int cras_client_get_attached_clients(const struct cras_client* client,
                                     struct cras_attached_client_info* clients,
                                     size_t max_clients) {
  const struct cras_server_state* state;
  unsigned num, version;
  int lock_rc;

  lock_rc = server_state_rdlock(client);
  if (lock_rc) {
    return -EINVAL;
  }
  state = client->server_state;

read_clients_again:
  version = begin_server_state_read(state);
  num = MIN(max_clients, state->num_attached_clients);
  memcpy(clients, state->client_info, num * sizeof(*clients));
  if (end_server_state_read(state, version)) {
    goto read_clients_again;
  }
  server_state_unlock(client, lock_rc);

  return num;
}

/* Find an output ionode on an iodev with the matching name.
 *
 * Args:
 *    dev_name - The prefix of the iodev name.
 *    node_name - The prefix of the ionode name.
 *    dev_info - The information about the iodev will be returned here.
 *    node_info - The information about the ionode will be returned here.
 * Returns:
 *    0 if successful, -1 if the node cannot be found.
 */
static int cras_client_find_output_node(const struct cras_client* client,
                                        const char* dev_name,
                                        const char* node_name,
                                        struct cras_iodev_info* dev_info,
                                        struct cras_ionode_info* node_info) {
  size_t ndevs, nnodes;
  struct cras_iodev_info* devs = NULL;
  struct cras_ionode_info* nodes = NULL;
  int rc = -1;
  unsigned i, j;

  if (!client || !dev_name || !node_name) {
    goto quit;
  }

  devs = (struct cras_iodev_info*)malloc(CRAS_MAX_IODEVS * sizeof(*devs));
  if (!devs) {
    goto quit;
  }

  nodes = (struct cras_ionode_info*)malloc(CRAS_MAX_IONODES * sizeof(*nodes));
  if (!nodes) {
    goto quit;
  }

  ndevs = CRAS_MAX_IODEVS;
  nnodes = CRAS_MAX_IONODES;
  rc = cras_client_get_output_devices(client, devs, nodes, &ndevs, &nnodes);
  if (rc < 0) {
    goto quit;
  }

  for (i = 0; i < ndevs; i++) {
    if (!strncmp(dev_name, devs[i].name, strlen(dev_name))) {
      goto found_dev;
    }
  }
  rc = -1;
  goto quit;

found_dev:
  for (j = 0; j < nnodes; j++) {
    if (nodes[j].iodev_idx == devs[i].idx &&
        !strncmp(node_name, nodes[j].name, strlen(node_name))) {
      goto found_node;
    }
  }
  rc = -1;
  goto quit;

found_node:
  *dev_info = devs[i];
  *node_info = nodes[j];
  rc = 0;

quit:
  free(devs);
  free(nodes);
  return rc;
}

int cras_client_get_node_by_id(const struct cras_client* client,
                               int input,
                               const cras_node_id_t node_id,
                               struct cras_ionode_info* node_info) {
  size_t ndevs, nnodes;
  struct cras_iodev_info* devs = NULL;
  struct cras_ionode_info* nodes = NULL;
  int rc = -EINVAL;
  unsigned i;

  if (!client || !node_info) {
    rc = -EINVAL;
    goto quit;
  }

  devs = (struct cras_iodev_info*)malloc(CRAS_MAX_IODEVS * sizeof(*devs));
  if (!devs) {
    rc = -ENOMEM;
    goto quit;
  }

  nodes = (struct cras_ionode_info*)malloc(CRAS_MAX_IONODES * sizeof(*nodes));
  if (!nodes) {
    rc = -ENOMEM;
    goto quit;
  }

  ndevs = CRAS_MAX_IODEVS;
  nnodes = CRAS_MAX_IONODES;
  if (input) {
    rc = cras_client_get_input_devices(client, devs, nodes, &ndevs, &nnodes);
  } else {
    rc = cras_client_get_output_devices(client, devs, nodes, &ndevs, &nnodes);
  }
  if (rc < 0) {
    goto quit;
  }

  rc = -ENOENT;
  for (i = 0; i < nnodes; i++) {
    if (node_id == cras_make_node_id(nodes[i].iodev_idx, nodes[i].ionode_idx)) {
      memcpy(node_info, &nodes[i], sizeof(*node_info));
      rc = 0;
      break;
    }
  }

quit:
  free(devs);
  free(nodes);
  return rc;
}

int cras_client_output_dev_plugged(const struct cras_client* client,
                                   const char* name) {
  struct cras_iodev_info dev_info;
  struct cras_ionode_info node_info = {0};

  if (cras_client_find_output_node(client, name, "Front Headphone Jack",
                                   &dev_info, &node_info) < 0) {
    return 0;
  }

  return node_info.plugged;
}

int cras_client_set_node_attr(struct cras_client* client,
                              cras_node_id_t node_id,
                              enum ionode_attr attr,
                              int value) {
  struct cras_set_node_attr msg;

  if (client == NULL) {
    return -EINVAL;
  }

  cras_fill_set_node_attr(&msg, node_id, attr, value);
  return write_message_to_server(client, &msg.header);
}

int cras_client_select_node(struct cras_client* client,
                            enum CRAS_STREAM_DIRECTION direction,
                            cras_node_id_t node_id) {
  struct cras_select_node msg;

  if (client == NULL) {
    return -EINVAL;
  }

  cras_fill_select_node(&msg, direction, node_id);
  return write_message_to_server(client, &msg.header);
}

int cras_client_add_active_node(struct cras_client* client,
                                enum CRAS_STREAM_DIRECTION direction,
                                cras_node_id_t node_id) {
  struct cras_add_active_node msg;

  if (client == NULL) {
    return -EINVAL;
  }

  cras_fill_add_active_node(&msg, direction, node_id);
  return write_message_to_server(client, &msg.header);
}

int cras_client_rm_active_node(struct cras_client* client,
                               enum CRAS_STREAM_DIRECTION direction,
                               cras_node_id_t node_id) {
  struct cras_rm_active_node msg;

  if (client == NULL) {
    return -EINVAL;
  }

  cras_fill_rm_active_node(&msg, direction, node_id);
  return write_message_to_server(client, &msg.header);
}

int cras_client_format_bytes_per_frame(struct cras_audio_format* fmt) {
  if (fmt == NULL) {
    return -EINVAL;
  }

  return cras_get_format_bytes(fmt);
}

int cras_client_calc_playback_latency(const struct timespec* sample_time,
                                      struct timespec* delay) {
  struct timespec now;

  if (delay == NULL) {
    return -EINVAL;
  }

  clock_gettime(CLOCK_MONOTONIC_RAW, &now);

  // for output return time until sample is played (t - now)
  subtract_timespecs(sample_time, &now, delay);
  return 0;
}

int cras_client_calc_capture_latency(const struct timespec* sample_time,
                                     struct timespec* delay) {
  struct timespec now;

  if (delay == NULL) {
    return -EINVAL;
  }

  clock_gettime(CLOCK_MONOTONIC_RAW, &now);

  // For input want time since sample read (now - t)
  subtract_timespecs(&now, sample_time, delay);
  return 0;
}

int cras_client_reload_dsp(struct cras_client* client) {
  struct cras_reload_dsp msg;

  if (client == NULL) {
    return -EINVAL;
  }

  cras_fill_reload_dsp(&msg);
  return write_message_to_server(client, &msg.header);
}

int cras_client_dump_dsp_info(struct cras_client* client) {
  struct cras_dump_dsp_info msg;

  if (client == NULL) {
    return -EINVAL;
  }

  cras_fill_dump_dsp_info(&msg);
  return write_message_to_server(client, &msg.header);
}

int cras_client_update_audio_debug_info(
    struct cras_client* client,
    void (*debug_info_cb)(struct cras_client*)) {
  struct cras_dump_audio_thread msg;

  if (client == NULL) {
    return -EINVAL;
  }

  if (client->debug_info_callback != NULL) {
    return -EINVAL;
  }
  client->debug_info_callback = debug_info_cb;

  cras_fill_dump_audio_thread(&msg);
  return write_message_to_server(client, &msg.header);
}

int cras_client_get_atlog_access(struct cras_client* client,
                                 void (*atlog_access_cb)(struct cras_client*)) {
  struct cras_get_atlog_fd msg;

  if (client == NULL) {
    return -EINVAL;
  }

  if (client->atlog_access_callback != NULL) {
    return -EINVAL;
  }
  client->atlog_access_callback = atlog_access_cb;

  cras_fill_get_atlog_fd(&msg);
  return write_message_to_server(client, &msg.header);
}

int cras_client_read_atlog(struct cras_client* client,
                           uint64_t* read_idx,
                           uint64_t* missing,
                           struct audio_thread_event_log* buf) {
  struct audio_thread_event_log log;
  uint64_t i, sync_write_pos, len = 0;
  struct timespec timestamp, last_timestamp;

  if (!client->atlog_ro) {
    return -EINVAL;
  }

  sync_write_pos = client->atlog_ro->sync_write_pos;
  __sync_synchronize();
  memcpy(&log, client->atlog_ro, sizeof(log));

  if (sync_write_pos <= *read_idx) {
    return 0;
  }

  *missing = 0;
  for (i = sync_write_pos - 1; i >= *read_idx; --i) {
    uint64_t pos = i % log.len;
    timestamp.tv_sec = log.log[pos].tag_sec & 0x00ffffff;
    timestamp.tv_nsec = log.log[pos].nsec;

    if (i != sync_write_pos - 1 &&
        timespec_after(&timestamp, &last_timestamp)) {
      if (*read_idx) {
        *missing = i - *read_idx + 1;
      }
      *read_idx = i + 1;
      break;
    }
    last_timestamp = timestamp;

    if (!i) {
      break;
    }
  }

  // Copies the continuous part of log.
  if ((sync_write_pos - 1) % log.len < *read_idx % log.len) {
    len = log.len - *read_idx % log.len;
    memcpy(buf->log, &log.log[*read_idx % log.len],
           sizeof(struct audio_thread_event) * len);
    memcpy(&buf->log[len], log.log,
           sizeof(struct audio_thread_event) *
               ((sync_write_pos - 1) % log.len + 1));
    len = sync_write_pos - *read_idx;
  } else {
    len = sync_write_pos - *read_idx;
    memcpy(buf->log, &log.log[*read_idx % log.len],
           sizeof(struct audio_thread_event) * len);
  }

  *read_idx = sync_write_pos;
  return len;
}

int cras_client_update_main_thread_debug_info(
    struct cras_client* client,
    void (*debug_info_cb)(struct cras_client*)) {
  struct cras_dump_main msg;

  if (client == NULL) {
    return -EINVAL;
  }
  if (client->debug_info_callback != NULL) {
    return -EINVAL;
  }
  client->debug_info_callback = debug_info_cb;
  cras_fill_dump_main(&msg);
  return write_message_to_server(client, &msg.header);
}

int cras_client_update_bt_debug_info(
    struct cras_client* client,
    void (*debug_info_cb)(struct cras_client*)) {
  struct cras_dump_bt msg;

  if (client == NULL) {
    return -EINVAL;
  }

  if (client->debug_info_callback != NULL) {
    return -EINVAL;
  }
  client->debug_info_callback = debug_info_cb;

  cras_fill_dump_bt(&msg);
  return write_message_to_server(client, &msg.header);
}

int cras_client_update_audio_thread_snapshots(
    struct cras_client* client,
    void (*debug_info_cb)(struct cras_client*)) {
  struct cras_dump_snapshots msg;

  if (client == NULL) {
    return -EINVAL;
  }

  if (client->debug_info_callback != NULL) {
    return -EINVAL;
  }
  client->debug_info_callback = debug_info_cb;

  cras_fill_dump_snapshots(&msg);
  return write_message_to_server(client, &msg.header);
}

int cras_client_get_max_supported_channels(const struct cras_client* client,
                                           cras_node_id_t node_id,
                                           uint32_t* max_channels) {
  size_t ndevs, nnodes;
  struct cras_iodev_info* devs = NULL;
  struct cras_ionode_info* nodes = NULL;
  int rc = -EINVAL;
  unsigned i;

  if (!client) {
    rc = -EINVAL;
    goto quit;
  }

  devs = (struct cras_iodev_info*)malloc(CRAS_MAX_IODEVS * sizeof(*devs));
  if (!devs) {
    rc = -ENOMEM;
    goto quit;
  }

  nodes = (struct cras_ionode_info*)malloc(CRAS_MAX_IONODES * sizeof(*nodes));
  if (!nodes) {
    rc = -ENOMEM;
    goto quit;
  }

  ndevs = CRAS_MAX_IODEVS;
  nnodes = CRAS_MAX_IONODES;
  rc = cras_client_get_output_devices(client, devs, nodes, &ndevs, &nnodes);
  if (rc < 0) {
    goto quit;
  }

  rc = -ENOENT;
  uint32_t iodev_idx;
  for (i = 0; i < nnodes; i++) {
    if (node_id == cras_make_node_id(nodes[i].iodev_idx, nodes[i].ionode_idx)) {
      iodev_idx = nodes[i].iodev_idx;
      rc = 0;
      break;
    }
  }

  if (rc < 0) {
    goto quit;
  }

  rc = -ENOENT;
  for (i = 0; i < ndevs; i++) {
    if (iodev_idx == devs[i].idx) {
      *max_channels = devs[i].max_supported_channels;
      rc = 0;
      break;
    }
  }

quit:
  free(devs);
  free(nodes);
  return rc;
}

int cras_client_set_node_volume(struct cras_client* client,
                                cras_node_id_t node_id,
                                uint8_t volume) {
  struct cras_set_node_attr msg;

  if (client == NULL) {
    return -EINVAL;
  }

  cras_fill_set_node_attr(&msg, node_id, IONODE_ATTR_VOLUME, volume);
  return write_message_to_server(client, &msg.header);
}

int cras_client_swap_node_left_right(struct cras_client* client,
                                     cras_node_id_t node_id,
                                     int enable) {
  struct cras_set_node_attr msg;

  if (client == NULL) {
    return -EINVAL;
  }

  cras_fill_set_node_attr(&msg, node_id, IONODE_ATTR_SWAP_LEFT_RIGHT, enable);
  return write_message_to_server(client, &msg.header);
}

int cras_client_set_node_capture_gain(struct cras_client* client,
                                      cras_node_id_t node_id,
                                      long gain) {
  struct cras_set_node_attr msg;

  if (client == NULL) {
    return -EINVAL;
  }
  if (gain > INT_MAX || gain < INT_MIN) {
    return -EINVAL;
  }

  cras_fill_set_node_attr(&msg, node_id, IONODE_ATTR_CAPTURE_GAIN, gain);
  return write_message_to_server(client, &msg.header);
}

int cras_client_add_test_iodev(struct cras_client* client,
                               enum TEST_IODEV_TYPE type) {
  struct cras_add_test_dev msg;

  cras_fill_add_test_dev(&msg, type);
  return write_message_to_server(client, &msg.header);
}

int cras_client_get_first_node_type_idx(const struct cras_client* client,
                                        enum CRAS_NODE_TYPE type,
                                        enum CRAS_STREAM_DIRECTION direction,
                                        cras_node_id_t* node_id) {
  const struct cras_server_state* state;
  unsigned int version;
  unsigned int i;
  const struct cras_ionode_info* node_list;
  unsigned int num_nodes;
  int lock_rc;

  lock_rc = server_state_rdlock(client);
  if (lock_rc) {
    return -EINVAL;
  }
  state = client->server_state;

read_nodes_again:
  version = begin_server_state_read(state);
  if (direction == CRAS_STREAM_OUTPUT) {
    node_list = state->output_nodes;
    num_nodes = state->num_output_nodes;
  } else {
    node_list = state->input_nodes;
    num_nodes = state->num_input_nodes;
  }
  for (i = 0; i < num_nodes; i++) {
    if ((enum CRAS_NODE_TYPE)node_list[i].type_enum == type) {
      *node_id =
          cras_make_node_id(node_list[i].iodev_idx, node_list[i].ionode_idx);
      server_state_unlock(client, lock_rc);
      return 0;
    }
  }
  if (end_server_state_read(state, version)) {
    goto read_nodes_again;
  }
  server_state_unlock(client, lock_rc);

  return -ENODEV;
}

int cras_client_get_first_dev_type_idx(const struct cras_client* client,
                                       enum CRAS_NODE_TYPE type,
                                       enum CRAS_STREAM_DIRECTION direction) {
  cras_node_id_t node_id;
  int rc;

  rc = cras_client_get_first_node_type_idx(client, type, direction, &node_id);
  if (rc) {
    return rc;
  }

  return dev_index_of(node_id);
}

int cras_client_set_suspend(struct cras_client* client, int suspend) {
  struct cras_server_message msg;

  cras_fill_suspend_message(&msg, suspend);
  return write_message_to_server(client, &msg);
}

int cras_client_get_hotword_models(struct cras_client* client,
                                   cras_node_id_t node_id,
                                   get_hotword_models_cb_t cb) {
  struct cras_get_hotword_models msg;

  if (!client) {
    return -EINVAL;
  }
  client->get_hotword_models_cb = cb;

  cras_fill_get_hotword_models_message(&msg, node_id);
  return write_message_to_server(client, &msg.header);
}

int cras_client_set_hotword_model(struct cras_client* client,
                                  cras_node_id_t node_id,
                                  const char* model_name) {
  struct cras_set_hotword_model msg;

  cras_fill_set_hotword_model_message(&msg, node_id, model_name);
  return write_message_to_server(client, &msg.header);
}

int cras_client_set_aec_dump(struct cras_client* client,
                             cras_stream_id_t stream_id,
                             int start,
                             int fd) {
  struct cras_set_aec_dump msg;

  cras_fill_set_aec_dump_message(&msg, stream_id, start);

  if (fd != -1) {
    return cras_send_with_fds(client->server_fd, &msg, sizeof(msg), &fd, 1);
  } else {
    return write_message_to_server(client, &msg.header);
  }
}

int cras_client_reload_aec_config(struct cras_client* client) {
  struct cras_reload_aec_config msg;

  cras_fill_reload_aec_config(&msg);
  return write_message_to_server(client, &msg.header);
}

int cras_client_get_aec_supported(struct cras_client* client) {
  int aec_supported;
  int lock_rc;

  lock_rc = server_state_rdlock(client);
  if (lock_rc) {
    return 0;
  }

  aec_supported = client->server_state->aec_supported;
  server_state_unlock(client, lock_rc);
  return aec_supported;
}

int cras_client_get_agc_supported(struct cras_client* client) {
  int agc_supported;
  int lock_rc;

  lock_rc = server_state_rdlock(client);
  if (lock_rc) {
    return 0;
  }

  agc_supported = client->server_state->agc_supported;
  server_state_unlock(client, lock_rc);
  return agc_supported;
}

int cras_client_get_ns_supported(struct cras_client* client) {
  int ns_supported;
  int lock_rc;

  lock_rc = server_state_rdlock(client);
  if (lock_rc) {
    return 0;
  }

  ns_supported = client->server_state->ns_supported;
  server_state_unlock(client, lock_rc);
  return ns_supported;
}

int cras_client_get_aec_group_id(struct cras_client* client) {
  int aec_group_id;
  int lock_rc;

  lock_rc = server_state_rdlock(client);
  if (lock_rc) {
    return -1;
  }

  aec_group_id = client->server_state->aec_group_id;
  server_state_unlock(client, lock_rc);
  return aec_group_id;
}

int cras_client_set_bt_wbs_enabled(struct cras_client* client, bool enabled) {
  struct cras_set_bt_wbs_enabled msg;

  cras_fill_set_bt_wbs_enabled(&msg, enabled);
  return write_message_to_server(client, &msg.header);
}

void cras_client_set_state_change_callback_context(struct cras_client* client,
                                                   void* context) {
  if (!client) {
    return;
  }
  client->observer_context = context;
}

static int cras_send_register_notification(struct cras_client* client,
                                           enum CRAS_CLIENT_MESSAGE_ID msg_id,
                                           int do_register) {
  struct cras_register_notification msg;
  int rc;

  /* This library automatically re-registers notifications when
   * reconnecting, so we can ignore message send failure due to no
   * connection. */
  cras_fill_register_notification_message(&msg, msg_id, do_register);
  rc = write_message_to_server(client, &msg.header);
  if (rc == -EPIPE) {
    rc = 0;
  }
  return rc;
}

int cras_client_set_output_volume_changed_callback(
    struct cras_client* client,
    cras_client_output_volume_changed_callback cb) {
  if (!client) {
    return -EINVAL;
  }
  client->observer_ops.output_volume_changed = cb;
  return cras_send_register_notification(
      client, CRAS_CLIENT_OUTPUT_VOLUME_CHANGED, cb != NULL);
}

int cras_client_set_output_mute_changed_callback(
    struct cras_client* client,
    cras_client_output_mute_changed_callback cb) {
  if (!client) {
    return -EINVAL;
  }
  client->observer_ops.output_mute_changed = cb;
  return cras_send_register_notification(
      client, CRAS_CLIENT_OUTPUT_MUTE_CHANGED, cb != NULL);
}

int cras_client_set_capture_gain_changed_callback(
    struct cras_client* client,
    cras_client_capture_gain_changed_callback cb) {
  if (!client) {
    return -EINVAL;
  }
  client->observer_ops.capture_gain_changed = cb;
  return cras_send_register_notification(
      client, CRAS_CLIENT_CAPTURE_GAIN_CHANGED, cb != NULL);
}

int cras_client_set_capture_mute_changed_callback(
    struct cras_client* client,
    cras_client_capture_mute_changed_callback cb) {
  if (!client) {
    return -EINVAL;
  }
  client->observer_ops.capture_mute_changed = cb;
  return cras_send_register_notification(
      client, CRAS_CLIENT_CAPTURE_MUTE_CHANGED, cb != NULL);
}

int cras_client_set_nodes_changed_callback(
    struct cras_client* client,
    cras_client_nodes_changed_callback cb) {
  if (!client) {
    return -EINVAL;
  }
  client->observer_ops.nodes_changed = cb;
  return cras_send_register_notification(client, CRAS_CLIENT_NODES_CHANGED,
                                         cb != NULL);
}

int cras_client_set_active_node_changed_callback(
    struct cras_client* client,
    cras_client_active_node_changed_callback cb) {
  if (!client) {
    return -EINVAL;
  }
  client->observer_ops.active_node_changed = cb;
  return cras_send_register_notification(
      client, CRAS_CLIENT_ACTIVE_NODE_CHANGED, cb != NULL);
}

int cras_client_set_output_node_volume_changed_callback(
    struct cras_client* client,
    cras_client_output_node_volume_changed_callback cb) {
  if (!client) {
    return -EINVAL;
  }
  client->observer_ops.output_node_volume_changed = cb;
  return cras_send_register_notification(
      client, CRAS_CLIENT_OUTPUT_NODE_VOLUME_CHANGED, cb != NULL);
}

int cras_client_set_node_left_right_swapped_changed_callback(
    struct cras_client* client,
    cras_client_node_left_right_swapped_changed_callback cb) {
  if (!client) {
    return -EINVAL;
  }
  client->observer_ops.node_left_right_swapped_changed = cb;
  return cras_send_register_notification(
      client, CRAS_CLIENT_NODE_LEFT_RIGHT_SWAPPED_CHANGED, cb != NULL);
}

int cras_client_set_input_node_gain_changed_callback(
    struct cras_client* client,
    cras_client_input_node_gain_changed_callback cb) {
  if (!client) {
    return -EINVAL;
  }
  client->observer_ops.input_node_gain_changed = cb;
  return cras_send_register_notification(
      client, CRAS_CLIENT_INPUT_NODE_GAIN_CHANGED, cb != NULL);
}

int cras_client_set_num_active_streams_changed_callback(
    struct cras_client* client,
    cras_client_num_active_streams_changed_callback cb) {
  if (!client) {
    return -EINVAL;
  }
  client->observer_ops.num_active_streams_changed = cb;
  return cras_send_register_notification(
      client, CRAS_CLIENT_NUM_ACTIVE_STREAMS_CHANGED, cb != NULL);
}

static int reregister_notifications(struct cras_client* client) {
  int rc;

  if (client->observer_ops.output_volume_changed) {
    rc = cras_client_set_output_volume_changed_callback(
        client, client->observer_ops.output_volume_changed);
    if (rc != 0) {
      return rc;
    }
  }
  if (client->observer_ops.output_mute_changed) {
    rc = cras_client_set_output_mute_changed_callback(
        client, client->observer_ops.output_mute_changed);
    if (rc != 0) {
      return rc;
    }
  }
  if (client->observer_ops.capture_gain_changed) {
    rc = cras_client_set_capture_gain_changed_callback(
        client, client->observer_ops.capture_gain_changed);
    if (rc != 0) {
      return rc;
    }
  }
  if (client->observer_ops.capture_mute_changed) {
    rc = cras_client_set_capture_mute_changed_callback(
        client, client->observer_ops.capture_mute_changed);
    if (rc != 0) {
      return rc;
    }
  }
  if (client->observer_ops.nodes_changed) {
    rc = cras_client_set_nodes_changed_callback(
        client, client->observer_ops.nodes_changed);
    if (rc != 0) {
      return rc;
    }
  }
  if (client->observer_ops.active_node_changed) {
    rc = cras_client_set_active_node_changed_callback(
        client, client->observer_ops.active_node_changed);
    if (rc != 0) {
      return rc;
    }
  }
  if (client->observer_ops.output_node_volume_changed) {
    rc = cras_client_set_output_node_volume_changed_callback(
        client, client->observer_ops.output_node_volume_changed);
    if (rc != 0) {
      return rc;
    }
  }
  if (client->observer_ops.node_left_right_swapped_changed) {
    rc = cras_client_set_node_left_right_swapped_changed_callback(
        client, client->observer_ops.node_left_right_swapped_changed);
    if (rc != 0) {
      return rc;
    }
  }
  if (client->observer_ops.input_node_gain_changed) {
    rc = cras_client_set_input_node_gain_changed_callback(
        client, client->observer_ops.input_node_gain_changed);
    if (rc != 0) {
      return rc;
    }
  }
  if (client->observer_ops.num_active_streams_changed) {
    rc = cras_client_set_num_active_streams_changed_callback(
        client, client->observer_ops.num_active_streams_changed);
    if (rc != 0) {
      return rc;
    }
  }
  return 0;
}

static int hotword_read_cb(struct cras_client* client,
                           cras_stream_id_t stream_id,
                           uint8_t* captured_samples,
                           uint8_t* playback_samples,
                           unsigned int frames,
                           const struct timespec* captured_time,
                           const struct timespec* playback_time,
                           void* user_arg) {
  struct cras_hotword_handle* handle;

  handle = (struct cras_hotword_handle*)user_arg;
  if (handle->trigger_cb) {
    handle->trigger_cb(client, handle, handle->user_data);
  }

  return 0;
}

static int hotword_err_cb(struct cras_client* client,
                          cras_stream_id_t stream_id,
                          int error,
                          void* user_arg) {
  struct cras_hotword_handle* handle;

  handle = (struct cras_hotword_handle*)user_arg;
  if (handle->err_cb) {
    handle->err_cb(client, handle, error, handle->user_data);
  }

  return 0;
}

int cras_client_enable_hotword_callback(
    struct cras_client* client,
    void* user_data,
    cras_hotword_trigger_cb_t trigger_cb,
    cras_hotword_error_cb_t err_cb,
    struct cras_hotword_handle** handle_out) {
  struct cras_hotword_handle* handle;
  int ret = 0;

  if (!client) {
    return -EINVAL;
  }

  handle = (struct cras_hotword_handle*)calloc(1, sizeof(*handle));
  if (!handle) {
    return -ENOMEM;
  }

  handle->format =
      cras_audio_format_create(SND_PCM_FORMAT_S16_LE, HOTWORD_FRAME_RATE, 1);
  if (!handle->format) {
    ret = -ENOMEM;
    goto cleanup;
  }

  handle->params = cras_client_unified_params_create(
      CRAS_STREAM_INPUT, HOTWORD_BLOCK_SIZE, CRAS_STREAM_TYPE_DEFAULT,
      HOTWORD_STREAM | TRIGGER_ONLY, (void*)handle, hotword_read_cb,
      hotword_err_cb, handle->format);
  if (!handle->params) {
    ret = -ENOMEM;
    goto cleanup_format;
  }

  handle->trigger_cb = trigger_cb;
  handle->err_cb = err_cb;
  handle->user_data = user_data;

  ret = cras_client_add_stream(client, &handle->stream_id, handle->params);
  if (ret) {
    goto cleanup_params;
  }

  *handle_out = handle;
  return 0;

cleanup_params:
  cras_client_stream_params_destroy(handle->params);
cleanup_format:
  cras_audio_format_destroy(handle->format);
cleanup:
  free(handle);
  return ret;
}

int cras_client_disable_hotword_callback(struct cras_client* client,
                                         struct cras_hotword_handle* handle) {
  if (!client || !handle) {
    return -EINVAL;
  }

  cras_client_rm_stream(client, handle->stream_id);
  cras_audio_format_destroy(handle->format);
  cras_client_stream_params_destroy(handle->params);
  free(handle);
  return 0;
}

int get_nodes(struct cras_client* client,
              enum CRAS_STREAM_DIRECTION direction,
              struct libcras_node_info*** nodes,
              size_t* num) {
  struct cras_iodev_info iodevs[CRAS_MAX_IODEVS];
  struct cras_ionode_info ionodes[CRAS_MAX_IONODES];
  size_t num_devs = CRAS_MAX_IODEVS, num_nodes = CRAS_MAX_IONODES;
  int rc, i, j;

  *num = 0;
  if (direction == CRAS_STREAM_INPUT) {
    rc = cras_client_get_input_devices(client, iodevs, ionodes, &num_devs,
                                       &num_nodes);
  } else {
    rc = cras_client_get_output_devices(client, iodevs, ionodes, &num_devs,
                                        &num_nodes);
  }

  if (rc < 0) {
    syslog(LOG_WARNING, "Failed to get devices: %d", rc);
    return rc;
  }

  *nodes = (struct libcras_node_info**)calloc(
      num_nodes, sizeof(struct libcras_node_info*));

  for (i = 0; i < (int)num_devs; i++) {
    for (j = 0; j < (int)num_nodes; j++) {
      if (iodevs[i].idx != ionodes[j].iodev_idx) {
        continue;
      }
      (*nodes)[*num] = libcras_node_info_create(&iodevs[i], &ionodes[j]);
      if ((*nodes)[*num] == NULL) {
        rc = -errno;
        goto clean;
      }
      (*num)++;
    }
  }
  return 0;
clean:
  for (i = 0; i < (int)*num; i++) {
    libcras_node_info_destroy((*nodes)[i]);
  }
  free(*nodes);
  *nodes = NULL;
  *num = 0;
  return rc;
}

int get_default_output_buffer_size(struct cras_client* client, int* size) {
  int rc = cras_client_get_default_output_buffer_size(client);
  if (rc < 0) {
    return rc;
  }
  *size = rc;
  return 0;
}

int get_aec_group_id(struct cras_client* client, int* id) {
  int lock_rc;

  lock_rc = server_state_rdlock(client);
  if (lock_rc) {
    return -EINVAL;
  }

  *id = client->server_state->aec_group_id;
  server_state_unlock(client, lock_rc);
  return 0;
}

int get_aec_supported(struct cras_client* client, int* supported) {
  *supported = cras_client_get_aec_supported(client);
  return 0;
}

int get_agc_supported(struct cras_client* client, int* supported) {
  *supported = cras_client_get_agc_supported(client);
  return 0;
}

int get_ns_supported(struct cras_client* client, int* supported) {
  *supported = cras_client_get_ns_supported(client);
  return 0;
}

int get_system_muted(struct cras_client* client, int* muted) {
  *muted = cras_client_get_system_muted(client);
  return 0;
}

int get_system_capture_muted(struct cras_client* client, int* muted) {
  *muted = cras_client_get_system_capture_muted(client);
  return 0;
}

int get_loopback_dev_idx(struct cras_client* client, int* idx) {
  int rc = cras_client_get_first_dev_type_idx(
      client, CRAS_NODE_TYPE_POST_MIX_PRE_DSP, CRAS_STREAM_INPUT);
  if (rc < 0) {
    return rc;
  }
  *idx = rc;
  return 0;
}

int get_floop_dev_idx_by_client_types(struct cras_client* client,
                                      int64_t client_types_mask,
                                      int* idx) {
  int rc =
      cras_client_get_floop_dev_idx_by_client_types(client, client_types_mask);
  if (rc < 0) {
    return rc;
  }
  *idx = rc;
  return 0;
}

static int32_t request_floop(struct cras_client* client,
                             const struct cras_floop_params* params,
                             const struct timespec* timeout) {
  if (!client) {
    return -EINVAL;
  }

  int rc;
  struct floop_request req = {
      .mu = PTHREAD_MUTEX_INITIALIZER,
  };

  // Calculate deadline
  struct timespec deadline;
  clock_gettime(CLOCK_MONOTONIC, &deadline);
  add_timespecs(&deadline, timeout);

  // Set up cond var
  pthread_condattr_t condattr;
  rc = -pthread_condattr_init(&condattr);
  if (rc) {
    goto cleanup_mu;
  }
  rc = -pthread_condattr_setclock(&condattr, CLOCK_MONOTONIC);
  if (rc) {
    goto cleanup_condattr;
  }
  rc = -pthread_cond_init(&req.cond, &condattr);
  if (rc) {
    goto cleanup_condattr;
  }

  // Add the request to the waiting list
  pthread_mutex_lock(&client->floop_request_list_mu);
  DL_APPEND(client->floop_request_list, &req);
  pthread_mutex_unlock(&client->floop_request_list_mu);

  // Write request message to server
  // The request is tagged with the address of req which is unique
  // The server will return the result along with the unique tag
  struct cras_request_floop msg;
  cras_fill_request_floop(&msg, params, (uint64_t)(uintptr_t)&req);
  rc = write_message_to_server(client, &msg.header);
  if (rc < 0) {
    goto cleanup;
  }

  // Set result
  pthread_mutex_lock(&req.mu);
  if (!req.fullfilled) {
    pthread_cond_timedwait(&req.cond, &req.mu, &deadline);
  }
  if (req.fullfilled) {
    rc = req.response;
  } else {
    rc = -ETIMEDOUT;
  }
  pthread_mutex_unlock(&req.mu);

cleanup:
  // Remove the request from the waiting list
  pthread_mutex_lock(&client->floop_request_list_mu);
  DL_DELETE(client->floop_request_list, &req);
  pthread_mutex_unlock(&client->floop_request_list_mu);

  pthread_cond_destroy(&req.cond);
cleanup_condattr:
  pthread_condattr_destroy(&condattr);
cleanup_mu:
  pthread_mutex_destroy(&req.mu);

  return rc;
}

int32_t cras_client_get_floop_dev_idx_by_client_types(
    struct cras_client* client,
    int64_t client_types_mask) {
  const struct cras_floop_params params = {
      .client_types_mask = client_types_mask,
  };
  const struct timespec timeout = {.tv_sec = 3};
  return request_floop(client, &params, &timeout);
}

struct libcras_client* libcras_client_create() {
  struct libcras_client* client =
      (struct libcras_client*)calloc(1, sizeof(struct libcras_client));
  if (!client) {
    syslog(LOG_WARNING, "cras_client: calloc failed");
    return NULL;
  }
  if (cras_client_create(&client->client_)) {
    libcras_client_destroy(client);
    return NULL;
  }
  client->api_version = CRAS_API_VERSION;
  client->connect = cras_client_connect;
  client->connect_timeout = cras_client_connect_timeout;
  client->connected_wait = cras_client_connected_wait;
  client->run_thread = cras_client_run_thread;
  client->stop = cras_client_stop;
  client->add_pinned_stream = cras_client_add_pinned_stream;
  client->rm_stream = cras_client_rm_stream;
  client->set_aec_ref = cras_client_set_aec_ref;
  client->set_stream_volume = cras_client_set_stream_volume;
  client->get_nodes = get_nodes;
  client->get_default_output_buffer_size = get_default_output_buffer_size;
  client->get_aec_group_id = get_aec_group_id;
  client->get_aec_supported = get_aec_supported;
  client->get_system_muted = get_system_muted;
  client->get_system_capture_muted = get_system_capture_muted;
  client->set_system_mute = cras_client_set_system_mute;
  client->get_loopback_dev_idx = get_loopback_dev_idx;
  client->get_floop_dev_idx_by_client_types =
      cras_client_get_floop_dev_idx_by_client_types;
  client->set_aec_dump = cras_client_set_aec_dump;
  client->get_agc_supported = get_agc_supported;
  client->get_ns_supported = get_ns_supported;
  client->set_client_type = cras_client_set_client_type;
  return client;
}

void libcras_client_destroy(struct libcras_client* client) {
  cras_client_destroy(client->client_);
  free(client);
}

int stream_params_set(struct cras_stream_params* params,
                      enum CRAS_STREAM_DIRECTION direction,
                      size_t buffer_frames,
                      size_t cb_threshold,
                      enum CRAS_STREAM_TYPE stream_type,
                      enum CRAS_CLIENT_TYPE client_type,
                      uint32_t flags,
                      void* user_data,
                      libcras_stream_cb_t stream_cb,
                      cras_error_cb_t err_cb,
                      size_t rate,
                      snd_pcm_format_t format,
                      size_t num_channels) {
  params->direction = direction;
  params->buffer_frames = buffer_frames;
  params->cb_threshold = cb_threshold;
  params->stream_type = stream_type;
  params->client_type = client_type;
  params->flags = flags;
  params->user_data = user_data;
  params->stream_cb = stream_cb;
  params->err_cb = err_cb;
  params->format.frame_rate = rate;
  params->format.format = format;
  params->format.num_channels = num_channels;
  return 0;
}

int stream_params_set_channel_layout(struct cras_stream_params* params,
                                     int length,
                                     const int8_t* layout) {
  if (length != CRAS_CH_MAX) {
    return -EINVAL;
  }
  return cras_audio_format_set_channel_layout(&params->format, layout);
}

struct libcras_stream_params* libcras_stream_params_create() {
  struct libcras_stream_params* params = (struct libcras_stream_params*)calloc(
      1, sizeof(struct libcras_stream_params));
  if (!params) {
    syslog(LOG_WARNING, "cras_client: calloc failed");
    return NULL;
  }
  params->params_ =
      (struct cras_stream_params*)calloc(1, sizeof(struct cras_stream_params));
  if (params->params_ == NULL) {
    syslog(LOG_WARNING, "cras_client: calloc failed");
    free(params);
    return NULL;
  }
  params->api_version = CRAS_API_VERSION;
  params->set = stream_params_set;
  params->set_channel_layout = stream_params_set_channel_layout;
  params->enable_aec = cras_client_stream_params_enable_aec;
  params->enable_ns = cras_client_stream_params_enable_ns;
  params->enable_agc = cras_client_stream_params_enable_agc;
  params->allow_aec_on_dsp = cras_client_stream_params_allow_aec_on_dsp;
  params->allow_ns_on_dsp = cras_client_stream_params_allow_ns_on_dsp;
  params->allow_agc_on_dsp = cras_client_stream_params_allow_agc_on_dsp;
  params->enable_ignore_ui_gains =
      cras_client_stream_params_enable_ignore_ui_gains;
  return params;
}

void libcras_stream_params_destroy(struct libcras_stream_params* params) {
  free(params->params_);
  free(params);
}

struct cras_node_info {
  uint64_t id;
  uint32_t dev_idx;
  uint32_t node_idx;
  uint32_t max_supported_channels;
  bool plugged;
  bool active;
  char type[CRAS_NODE_TYPE_BUFFER_SIZE];
  char node_name[CRAS_NODE_NAME_BUFFER_SIZE];
  char dev_name[CRAS_IODEV_NAME_BUFFER_SIZE];
};

int cras_node_info_get_id(struct cras_node_info* node, uint64_t* id) {
  (*id) = node->id;
  return 0;
}

int cras_node_info_get_dev_idx(struct cras_node_info* node, uint32_t* dev_idx) {
  (*dev_idx) = node->dev_idx;
  return 0;
}

int cras_node_info_get_node_idx(struct cras_node_info* node,
                                uint32_t* node_idx) {
  (*node_idx) = node->node_idx;
  return 0;
}

int cras_node_info_get_max_supported_channels(
    struct cras_node_info* node,
    uint32_t* max_supported_channels) {
  (*max_supported_channels) = node->max_supported_channels;
  return 0;
}

int cras_node_info_is_plugged(struct cras_node_info* node, bool* is_plugged) {
  (*is_plugged) = node->plugged;
  return 0;
}

int cras_node_info_is_active(struct cras_node_info* node, bool* is_active) {
  (*is_active) = node->active;
  return 0;
}

int cras_node_info_get_type(struct cras_node_info* node, char** type) {
  (*type) = node->type;
  return 0;
}

int cras_node_info_get_node_name(struct cras_node_info* node,
                                 char** node_name) {
  (*node_name) = node->node_name;
  return 0;
}

int cras_node_info_get_dev_name(struct cras_node_info* node, char** dev_name) {
  (*dev_name) = node->dev_name;
  return 0;
}

struct libcras_node_info* libcras_node_info_create(
    struct cras_iodev_info* iodev,
    struct cras_ionode_info* ionode) {
  struct libcras_node_info* node =
      (struct libcras_node_info*)calloc(1, sizeof(struct libcras_node_info));
  if (!node) {
    syslog(LOG_WARNING, "cras_client: calloc failed");
    return NULL;
  }
  node->node_ =
      (struct cras_node_info*)calloc(1, sizeof(struct cras_node_info));
  if (node->node_ == NULL) {
    syslog(LOG_WARNING, "cras_client: calloc failed");
    free(node);
    return NULL;
  }
  node->api_version = CRAS_API_VERSION;
  node->node_->id = cras_make_node_id(ionode->iodev_idx, ionode->ionode_idx);
  node->node_->dev_idx = ionode->iodev_idx;
  node->node_->node_idx = ionode->ionode_idx;
  node->node_->max_supported_channels = iodev->max_supported_channels;
  node->node_->plugged = ionode->plugged;
  node->node_->active = ionode->active;
  strncpy(node->node_->type, ionode->type, CRAS_NODE_TYPE_BUFFER_SIZE);
  node->node_->type[CRAS_NODE_TYPE_BUFFER_SIZE - 1] = '\0';
  strncpy(node->node_->node_name, ionode->name, CRAS_NODE_NAME_BUFFER_SIZE);
  node->node_->node_name[CRAS_NODE_NAME_BUFFER_SIZE - 1] = '\0';
  strncpy(node->node_->dev_name, iodev->name, CRAS_IODEV_NAME_BUFFER_SIZE);
  node->node_->dev_name[CRAS_IODEV_NAME_BUFFER_SIZE - 1] = '\0';
  node->get_id = cras_node_info_get_id;
  node->get_dev_idx = cras_node_info_get_dev_idx;
  node->get_node_idx = cras_node_info_get_node_idx;
  node->get_max_supported_channels = cras_node_info_get_max_supported_channels;
  node->is_plugged = cras_node_info_is_plugged;
  node->is_active = cras_node_info_is_active;
  node->get_type = cras_node_info_get_type;
  node->get_node_name = cras_node_info_get_node_name;
  node->get_dev_name = cras_node_info_get_dev_name;
  return node;
}

void libcras_node_info_destroy(struct libcras_node_info* node) {
  free(node->node_);
  free(node);
}

void libcras_node_info_array_destroy(struct libcras_node_info** nodes,
                                     size_t num) {
  int i;
  for (i = 0; i < (int)num; i++) {
    libcras_node_info_destroy(nodes[i]);
  }
  free(nodes);
}
