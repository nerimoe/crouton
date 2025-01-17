/* Copyright 2012 The ChromiumOS Authors
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 *
 * This API creates multiple threads, one for control, and a thread per audio
 * stream. The control thread is used to receive messages and notifications
 * from the audio server, and manage the per-stream threads. API calls below
 * may send messages to the control thread, or directly to the server. It is
 * required that the control thread is running in order to support audio
 * streams and notifications from the server.
 *
 * The API has multiple initialization sequences, but some of those can block
 * while waiting for a response from the server.
 *
 * The following is the non-blocking API initialization sequence:
 *	cras_client_create()
 *      cras_client_set_connection_status_cb()                       (optional)
 *      cras_client_run_thread()
 *      cras_client_connect_async()
 *
 * The connection callback is executed asynchronously from the control thread
 * when the connection has been established. The connection callback should be
 * used to turn on or off interactions with any API call that communicates with
 * the audio server or starts/stops audio streams. The above is implemented by
 * cras_helper_create_connect_async().
 *
 * The following alternative (deprecated) initialization sequence can ensure
 * that the connection is established synchronously.
 *
 * Just connect to the server (no control thread):
 *      cras_client_create()
 *      cras_client_set_server_connection_cb()                       (optional)
 *   one of:
 *      cras_client_connect()                                  (blocks forever)
 *   or
 *      cras_client_connect_timeout()                      (blocks for timeout)
 *
 * For API calls below that require the control thread to be running:
 *      cras_client_run_thread();
 *      cras_client_connected_wait();                     (blocks up to 1 sec.)
 *
 * The above minus setting the connection callback is implemented within
 * cras_helper_create_connect().
 */

#ifndef CRAS_INCLUDE_CRAS_CLIENT_H_
#define CRAS_INCLUDE_CRAS_CLIENT_H_

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stdint.h>
#include <sys/select.h>

#include "cras_iodev_info.h"
#include "cras_types.h"
#include "cras_util.h"

struct cras_client;
struct cras_hotword_handle;
struct cras_stream_params;

/* Callback for audio received or transmitted.
 * Args (All pointer will be valid - except user_arg, that's up to the user):
 *    client: The client requesting service.
 *    stream_id - Unique identifier for the stream needing data read/written.
 *    samples - Read or write samples to/form here.
 *    frames - Maximum number of frames to read or write.
 *    sample_time - Playback time for the first sample read/written.
 *    user_arg - Value passed to add_stream;
 * Return:
 *    Returns the number of frames read or written on success, or a negative
 *    number if there is a stream-fatal error. Returns EOF when the end of the
 *    stream is reached.
 */
typedef int (*cras_playback_cb_t)(struct cras_client* client,
                                  cras_stream_id_t stream_id,
                                  uint8_t* samples,
                                  size_t frames,
                                  const struct timespec* sample_time,
                                  void* user_arg);

/* Callback for audio received and/or transmitted.
 * Args (All pointer will be valid - except user_arg, that's up to the user):
 *    client: The client requesting service.
 *    stream_id - Unique identifier for the stream needing data read/written.
 *    captured_samples - Read samples form here.
 *    playback_samples - Read or write samples to here.
 *    frames - Maximum number of frames to read or write.
 *    captured_time - Time the first sample was read.
 *    playback_time - Playback time for the first sample written.
 *    user_arg - Value passed to add_stream;
 * Return:
 *    Returns the number of frames read or written on success, or a negative
 *    number if there is a stream-fatal error. Returns EOF when the end of the
 *    stream is reached.
 */
typedef int (*cras_unified_cb_t)(struct cras_client* client,
                                 cras_stream_id_t stream_id,
                                 uint8_t* captured_samples,
                                 uint8_t* playback_samples,
                                 unsigned int frames,
                                 const struct timespec* captured_time,
                                 const struct timespec* playback_time,
                                 void* user_arg);

/* Callback for handling stream errors.
 * Args:
 *    client - The client created with cras_client_create().
 *    stream_id - The ID for this stream.
 *    error - The error code,
 *    user_arg - The argument defined in cras_client_*_params_create().
 */
typedef int (*cras_error_cb_t)(struct cras_client* client,
                               cras_stream_id_t stream_id,
                               int error,
                               void* user_arg);

// Server connection status.
typedef enum cras_connection_status {
  CRAS_CONN_STATUS_FAILED,
  /* Resource allocation problem. Free resources, and retry the
   * connection with cras_client_connect_async(), or (blocking)
   * cras_client_connect(). Do not call cras_client_connect(),
   * cras_client_connect_timeout(), or cras_client_destroy()
   * from the callback. */
  CRAS_CONN_STATUS_DISCONNECTED,
  /* The control thread is attempting to reconnect to the
   * server in the background. Any attempt to access the
   * server will fail or block (see
   * cras_client_set_server_message_blocking(). */
  CRAS_CONN_STATUS_CONNECTED,
  /* Connection is established. All state change callbacks
   * have been re-registered, but audio streams must be
   * restarted, and node state data must be updated. */
} cras_connection_status_t;

/* Callback for handling server connection status.
 *
 * See also cras_client_set_connection_status_cb(). Do not call
 * cras_client_connect(), cras_client_connect_timeout(), or
 * cras_client_destroy() from this callback.
 *
 * Args:
 *    client - The client created with cras_client_create().
 *    status - The status of the connection to the server.
 *    user_arg - The argument defined in
 *               cras_client_set_connection_status_cb().
 */
typedef void (*cras_connection_status_cb_t)(struct cras_client* client,
                                            cras_connection_status_t status,
                                            void* user_arg);

// Callback for setting thread priority.
typedef void (*cras_thread_priority_cb_t)(struct cras_client* client);

// Callback for handling get hotword models reply.
typedef void (*get_hotword_models_cb_t)(struct cras_client* client,
                                        const char* hotword_models);

// Callback to wait for a hotword trigger.
typedef void (*cras_hotword_trigger_cb_t)(struct cras_client* client,
                                          struct cras_hotword_handle* handle,
                                          void* user_data);

// Callback for handling hotword errors.
typedef int (*cras_hotword_error_cb_t)(struct cras_client* client,
                                       struct cras_hotword_handle* handle,
                                       int error,
                                       void* user_data);

/*
 * Client handling.
 */

/* Creates a new client.
 * Args:
 *    client - Filled with a pointer to the new client.
 * Returns:
 *    0 on success (*client is filled with a valid cras_client pointer).
 *    Negative error code on failure(*client will be NULL).
 */
int cras_client_create(struct cras_client** client);

/* Creates a new client with given connection type.
 * Args:
 *     client - Filled with a pointer to the new client.
 *     conn_type - enum CRAS_CONNECTION_TYPE
 *
 * Returns:
 *     0 on success (*client is filled with a valid cras_client pointer).
 *     Negative error code on failure(*client will be NULL).
 */
int cras_client_create_with_type(struct cras_client** client,
                                 enum CRAS_CONNECTION_TYPE conn_type);

/* Destroys a client.
 * Args:
 *    client - returned from "cras_client_create".
 */
void cras_client_destroy(struct cras_client* client);

/* Connects a client to the running server.
 * Waits forever (until interrupted or connected).
 * Args:
 *    client - pointer returned from "cras_client_create".
 * Returns:
 *    0 on success, or a negative error code on failure (from errno.h).
 */
int cras_client_connect(struct cras_client* client);

/* Connects a client to the running server, retries until timeout.
 * Args:
 *    client - pointer returned from "cras_client_create".
 *    timeout_ms - timeout in milliseconds or negative to wait forever.
 * Returns:
 *    0 on success, or a negative error code on failure (from errno.h).
 */
int cras_client_connect_timeout(struct cras_client* client,
                                unsigned int timeout_ms);

/* Begins running the client control thread.
 *
 * Required for stream operations and other operations noted below.
 *
 * Args:
 *    client - the client to start (from cras_client_create).
 * Returns:
 *    0 on success or if the thread is already running, -EINVAL if the client
 *    pointer is NULL, or the negative result of pthread_create().
 */
int cras_client_run_thread(struct cras_client* client);

/* Stops running a client.
 * This function is executed automatically by cras_client_destroy().
 * Args:
 *    client - the client to stop (from cras_client_create).
 * Returns:
 *    0 on success or if the thread was already stopped, -EINVAL if the client
 *    isn't valid.
 */
int cras_client_stop(struct cras_client* client);

/* Wait up to 1 second for the client thread to complete the server connection.
 *
 * After cras_client_run_thread() is executed, this function can be used to
 * ensure that the connection has been established with the server and ensure
 * that any information about the server is up to date. If
 * cras_client_run_thread() has not yet been executed, or cras_client_stop()
 * was executed and thread isn't running, then this function returns -EINVAL.
 *
 * Args:
 *    client - pointer returned from "cras_client_create".
 * Returns:
 *    0 on success, or a negative error code on failure (from errno.h).
 */
int cras_client_connected_wait(struct cras_client* client);

/* Ask the client control thread to connect to the audio server.
 *
 * After cras_client_run_thread() is executed, this function can be used
 * to ask the control thread to connect to the audio server asynchronously.
 * The callback set with cras_client_set_connection_status_cb() will be
 * executed when the connection is established.
 *
 * Args:
 *    client - The client from cras_client_create().
 * Returns:
 *    0 on success, or a negative error code on failure (from errno.h).
 *    -EINVAL if the client pointer is invalid or the control thread is
 *    not running.
 */
int cras_client_connect_async(struct cras_client* client);

/* Sets server connection status callback.
 *
 * See cras_connection_status_t for a description of the connection states
 * and appropriate user action.
 *
 * Args:
 *    client - The client from cras_client_create.
 *    connection_cb - The callback function to register.
 *    user_arg - Pointer that will be passed to the callback.
 */
void cras_client_set_connection_status_cb(
    struct cras_client* client,
    cras_connection_status_cb_t connection_cb,
    void* user_arg);

/* Sets callback for setting thread priority.
 * Args:
 *    client - The client from cras_client_create.
 *    cb - The thread priority callback.
 */
void cras_client_set_thread_priority_cb(struct cras_client* client,
                                        cras_thread_priority_cb_t cb);

/* Returns the current list of output devices.
 *
 * Requires that the connection to the server has been established.
 *
 * Data is copied and thus can become out of date. This call must be
 * re-executed to get updates.
 *
 * Args:
 *    client - The client from cras_client_create.
 *    devs - Array that will be filled with device info.
 *    nodes - Array that will be filled with node info.
 *    *num_devs - Maximum number of devices to put in the array.
 *    *num_nodes - Maximum number of nodes to put in the array.
 * Returns:
 *    0 on success, -EINVAL if the client isn't valid or isn't running.
 *    *num_devs is set to the actual number of devices info filled.
 *    *num_nodes is set to the actual number of nodes info filled.
 */
int cras_client_get_output_devices(const struct cras_client* client,
                                   struct cras_iodev_info* devs,
                                   struct cras_ionode_info* nodes,
                                   size_t* num_devs,
                                   size_t* num_nodes);

/* Returns the current list of input devices.
 *
 * Requires that the connection to the server has been established.
 *
 * Data is copied and thus can become out of date. This call must be
 * re-executed to get updates.
 *
 * Args:
 *    client - The client from cras_client_create.
 *    devs - Array that will be filled with device info.
 *    nodes - Array that will be filled with node info.
 *    *num_devs - Maximum number of devices to put in the array.
 *    *num_nodes - Maximum number of nodes to put in the array.
 * Returns:
 *    0 on success, -EINVAL if the client isn't valid or isn't running.
 *    *num_devs is set to the actual number of devices info filled.
 *    *num_nodes is set to the actual number of nodes info filled.
 */
int cras_client_get_input_devices(const struct cras_client* client,
                                  struct cras_iodev_info* devs,
                                  struct cras_ionode_info* nodes,
                                  size_t* num_devs,
                                  size_t* num_nodes);

/* Returns the current list of clients attached to the server.
 *
 * Requires that the connection to the server has been established.
 *
 * Data is copied and thus can become out of date. This call must be
 * re-executed to get updates.
 *
 * Args:
 *    client - This client (from cras_client_create).
 *    clients - Array that will be filled with a list of attached clients.
 *    max_clients - Maximum number of clients to put in the array.
 * Returns:
 *    The number of attached clients.  This may be more that max_clients passed
 *    in, this indicates that all of the clients wouldn't fit in the provided
 *    array.
 */
int cras_client_get_attached_clients(const struct cras_client* client,
                                     struct cras_attached_client_info* clients,
                                     size_t max_clients);

/* Find a node info with the matching node id.
 *
 * Requires that the connection to the server has been established.
 *
 * Data is copied and thus can become out of date. This call must be
 * re-executed to get updates.
 *
 * Args:
 *    client - This client (from cras_client_create).
 *    input - Non-zero for input nodes, zero for output nodes.
 *    node_id - The node id to look for.
 *    node_info - The information about the ionode will be returned here.
 * Returns:
 *    0 if successful, negative on error; -ENOENT if the node cannot be found.
 */
int cras_client_get_node_by_id(const struct cras_client* client,
                               int input,
                               const cras_node_id_t node_id,
                               struct cras_ionode_info* node_info);

/* Checks if the output device with the given name is currently plugged in.
 *
 * For internal devices this checks that jack state, for USB devices this will
 * always be true if they are present. The name parameter can be the complete
 * name or any unique prefix of the name. If the name is not unique the first
 * matching name will be checked.
 *
 * Requires that the connection to the server has been established.
 *
 * Data is copied and thus can become out of date. This call must be
 * re-executed to get updates.
 *
 * Args:
 *    client - The client from cras_client_create.
 *    name - Name of the device to check.
 * Returns:
 *    1 if the device exists and is plugged, 0 otherwise.
 */
int cras_client_output_dev_plugged(const struct cras_client* client,
                                   const char* name);

/* Set the value of an attribute of an ionode.
 *
 * Args:
 *    client - The client from cras_client_create.
 *    node_id - The id of the ionode.
 *    attr - the attribute we want to change.
 *    value - the value we want to set.
 * Returns:
 *    Returns 0 for success, negative on error (from errno.h).
 */
int cras_client_set_node_attr(struct cras_client* client,
                              cras_node_id_t node_id,
                              enum ionode_attr attr,
                              int value);

/* Select the preferred node for playback/capture.
 *
 * Args:
 *    client - The client from cras_client_create.
 *    direction - The direction of the ionode.
 *    node_id - The id of the ionode. If node_id is the special value 0, then
 *        the preference is cleared and cras will choose automatically.
 */
int cras_client_select_node(struct cras_client* client,
                            enum CRAS_STREAM_DIRECTION direction,
                            cras_node_id_t node_id);

/* Adds an active node for playback/capture.
 *
 * Args:
 *    client - The client from cras_client_create.
 *    direction - The direction of the ionode.
 *    node_id - The id of the ionode. If there's no node matching given
 *        id, nothing will happen in CRAS.
 */
int cras_client_add_active_node(struct cras_client* client,
                                enum CRAS_STREAM_DIRECTION direction,
                                cras_node_id_t node_id);

/* Removes an active node for playback/capture.
 *
 * Args:
 *    client - The client from cras_client_create.
 *    direction - The direction of the ionode.
 *    node_id - The id of the ionode. If there's no node matching given
 *        id, nothing will happen in CRAS.
 */
int cras_client_rm_active_node(struct cras_client* client,
                               enum CRAS_STREAM_DIRECTION direction,
                               cras_node_id_t node_id);

/* Asks the server to reload dsp plugin configuration from the ini file.
 *
 * Args:
 *    client - The client from cras_client_create.
 * Returns:
 *    0 on success, -EINVAL if the client isn't valid or isn't running.
 */
int cras_client_reload_dsp(struct cras_client* client);

/* Asks the server to dump current dsp information to syslog.
 *
 * Args:
 *    client - The client from cras_client_create.
 * Returns:
 *    0 on success, -EINVAL if the client isn't valid or isn't running.
 */
int cras_client_dump_dsp_info(struct cras_client* client);

/* Asks the server to dump current audio thread information.
 *
 * Args:
 *    client - The client from cras_client_create.
 *    cb - A function to call when the data is received.
 * Returns:
 *    0 on success, -EINVAL if the client isn't valid or isn't running.
 */
int cras_client_update_audio_debug_info(struct cras_client* client,
                                        void (*cb)(struct cras_client*));

/* Asks the server to dump current main thread information.
 * Args:
 *    client - The client from cras_client_create.
 *    cb - A function to call when the data is received.
 * Returns:
 *    0 on success, -EINVAL if the client isn't valid or isn't running.
 */
int cras_client_update_main_thread_debug_info(struct cras_client* client,
                                              void (*cb)(struct cras_client*));

/* Asks the server to dump bluetooth debug information.
 * Args:
 *    client - The client from cras_client_create.
 *    cb - Function to call when debug info is ready.
 * Returns:
 *    0 on success, -EINVAL if the client isn't valid or isn't running.
 */
int cras_client_update_bt_debug_info(struct cras_client* client,
                                     void (*cb)(struct cras_client*));

/* Gets read-only access to audio thread log. Should be called once before
   calling cras_client_read_atlog.
 * Args:
 *    client - The client from cras_client_create.
 *    atlog_access_cb - Function to call after getting atlog access.
 * Returns:
 *    0 on success, -EINVAL if the client or atlog_access_cb isn't valid.
 */
int cras_client_get_atlog_access(struct cras_client* client,
                                 void (*atlog_access_cb)(struct cras_client*));

/* Reads continuous audio thread log into 'buf', starting from 'read_idx'-th log
 * till the latest. The number of missing logs within the range will be stored
 * in 'missing'. Requires calling cras_client_get_atlog_access() beforehand
 * to get access to audio thread log.
 * Args:
 *    client - The client from cras_client_create.
 *    read_idx - The log number to start reading with.
 *    missing - The pointer to store the number of missing logs.
 *    buf - The buffer to which continuous logs will be copied.
 * Returns:
 *    The number of logs copied. < 0 if failed to read audio thread log.
 */
int cras_client_read_atlog(struct cras_client* client,
                           uint64_t* read_idx,
                           uint64_t* missing,
                           struct audio_thread_event_log* buf);

/* Asks the server to dump current audio thread snapshots.
 *
 * Args:
 *    client - The client from cras_client_create.
 *    cb - A function to call when the data is received.
 * Returns:
 *    0 on success, -EINVAL if the client isn't valid or isn't running.
 */
int cras_client_update_audio_thread_snapshots(struct cras_client* client,
                                              void (*cb)(struct cras_client*));

/* Gets the max supported channel count of the output device from node_id.
 * Args:
 *    client - The client from cras_client_create.
 *    node_id - ID of the node.
 *    max_channels - Out parameter will be filled with the max supported channel
 *        count.
 * Returns:
 *    0 on success, or negative error code on failure.
 */
int cras_client_get_max_supported_channels(const struct cras_client* client,
                                           cras_node_id_t node_id,
                                           uint32_t* max_channels);

/* Set the client type on the given client.
 *
 * This API is added to allow setting the client type before adding any stream.
 * When adding a new stream, if the client type in cras_stream_params is not set
 * (default to CRAS_CLIENT_TYPE_UNKNOWN), the client type set beforehand by this
 * function is used. Otherwise the client type in cras_stream_params temporarily
 * overrides the one set by this function, for this new stream only.
 *
 * The client type affects the type of iodevs reported by
 * cras_client_get_*_devices. CRAS_CLIENT_TYPE_TEST shows internal iodevs not
 * intended to be visible to end users. A stream's client type is used for
 * identifying streams for flexible loopback (floop), identifying RTC streams,
 * and providing info for metrics and debug dumps.
 *
 * Args:
 *    client - The client from cras_client_create.
 *    client_type - A client type.
 * Returns:
 *    0 on success, or negative error code on failure.
 */
int cras_client_set_client_type(struct cras_client* client,
                                enum CRAS_CLIENT_TYPE client_type);

/*
 * Stream handling.
 */

/* Setup stream configuration parameters.
 * Args:
 *    direction - playback(CRAS_STREAM_OUTPUT) or capture(CRAS_STREAM_INPUT).
 *    buffer_frames - total number of audio frames to buffer (dictates latency).
 *    cb_threshold - For playback, call back for more data when the buffer
 *        reaches this level. For capture, this is ignored (Audio callback will
 *        be called when buffer_frames have been captured).
 *    unused - No longer used.
 *    stream_type - media or talk (currently only support "default").
 *    flags - Currently only used for CRAS_INPUT_STREAM_FLAG.
 *    user_data - Pointer that will be passed to the callback.
 *    aud_cb - Called when audio is needed(playback) or ready(capture). Allowed
 *        return EOF to indicate that the stream should terminate.
 *    err_cb - Called when there is an error with the stream.
 *    format - The format of the audio stream.  Specifies bits per sample,
 *        number of channels, and sample rate.
 */
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
    struct cras_audio_format* format);

/* Functions to set the client type on given stream parameter. DEPRECATED.
 *
 * If the argument is not CRAS_CLIENT_TYPE_UNKNOWN, it will temporarily override
 * the current client type, for the new stream(s) created with this
 * cras_stream_params only. See cras_client_set_client_type.
 *
 * The client type is not expected to change frequently during the lifetime of
 * a client. Remove this function after all clients switch to use
 * cras_client_set_client_type.
 *
 * Args:
 *    params - Stream configuration parameters.
 *    client_type - A client type.
 */
void cras_client_stream_params_set_client_type(
    struct cras_stream_params* params,
    enum CRAS_CLIENT_TYPE client_type);

/* Functions to enable or disable specific effect on given stream parameter.
 * Args:
 *    params - Stream configuration parameters.
 */
void cras_client_stream_params_enable_aec(struct cras_stream_params* params);
void cras_client_stream_params_disable_aec(struct cras_stream_params* params);
void cras_client_stream_params_enable_ns(struct cras_stream_params* params);
void cras_client_stream_params_disable_ns(struct cras_stream_params* params);
void cras_client_stream_params_enable_agc(struct cras_stream_params* params);
void cras_client_stream_params_disable_agc(struct cras_stream_params* params);
void cras_client_stream_params_enable_vad(struct cras_stream_params* params);
void cras_client_stream_params_disable_vad(struct cras_stream_params* params);
void cras_client_stream_params_allow_aec_on_dsp(
    struct cras_stream_params* params);
void cras_client_stream_params_disallow_aec_on_dsp(
    struct cras_stream_params* params);
void cras_client_stream_params_allow_ns_on_dsp(
    struct cras_stream_params* params);
void cras_client_stream_params_disallow_ns_on_dsp(
    struct cras_stream_params* params);
void cras_client_stream_params_allow_agc_on_dsp(
    struct cras_stream_params* params);
void cras_client_stream_params_disallow_agc_on_dsp(
    struct cras_stream_params* params);
void cras_client_stream_params_enable_ignore_ui_gains(
    struct cras_stream_params* params);
void cras_client_stream_params_disable_ignore_ui_gains(
    struct cras_stream_params* params);

/* Setup stream configuration parameters. DEPRECATED.
 * TODO(crbug.com/972928): remove this
 * Use cras_client_stream_params_create instead.
 * Args:
 *    direction - playback(CRAS_STREAM_OUTPUT) or capture(CRAS_STREAM_INPUT) or
 *        loopback(CRAS_STREAM_POST_MIX_PRE_DSP).
 *    block_size - The number of frames per callback(dictates latency).
 *    stream_type - media or talk (currently only support "default").
 *    flags - None currently used.
 *    user_data - Pointer that will be passed to the callback.
 *    unified_cb - Called to request audio data or to notify the client when
 *                 captured audio is available. Though this is a unified_cb,
 *                 only one direction will be used for a stream, depending
 *                 on the 'direction' parameter.
 *    err_cb - Called when there is an error with the stream.
 *    format - The format of the audio stream.  Specifies bits per sample,
 *        number of channels, and sample rate.
 */
struct cras_stream_params* cras_client_unified_params_create(
    enum CRAS_STREAM_DIRECTION direction,
    unsigned int block_size,
    enum CRAS_STREAM_TYPE stream_type,
    uint32_t flags,
    void* user_data,
    cras_unified_cb_t unified_cb,
    cras_error_cb_t err_cb,
    struct cras_audio_format* format);

// Destroy stream params created with cras_client_stream_params_create.
void cras_client_stream_params_destroy(struct cras_stream_params* params);

/* Creates a new stream and return the stream id or < 0 on error.
 *
 * Requires execution of cras_client_run_thread(), and an active connection
 * to the audio server.
 *
 * Args:
 *    client - The client to add the stream to (from cras_client_create).
 *    stream_id_out - On success will be filled with the new stream id.
 *        Guaranteed to be set before any callbacks are made.
 *    config - The cras_stream_params struct specifying the parameters for the
 *        stream.
 * Returns:
 *    0 on success, negative error code on failure (from errno.h).
 */
int cras_client_add_stream(struct cras_client* client,
                           cras_stream_id_t* stream_id_out,
                           struct cras_stream_params* config);

/* Creates a pinned stream and return the stream id or < 0 on error.
 *
 * Requires execution of cras_client_run_thread(), and an active connection
 * to the audio server.
 *
 * Args:
 *    client - The client to add the stream to (from cras_client_create).
 *    dev_idx - Index of the device to attach the newly created stream.
 *    stream_id_out - On success will be filled with the new stream id.
 *        Guaranteed to be set before any callbacks are made.
 *    config - The cras_stream_params struct specifying the parameters for the
 *        stream.
 * Returns:
 *    0 on success, negative error code on failure (from errno.h).
 */
int cras_client_add_pinned_stream(struct cras_client* client,
                                  uint32_t dev_idx,
                                  cras_stream_id_t* stream_id_out,
                                  struct cras_stream_params* config);

/* Removes a currently playing/capturing stream.
 *
 * Requires execution of cras_client_run_thread().
 *
 * Args:
 *    client - Client to remove the stream (returned from cras_client_create).
 *    stream_id - ID returned from cras_client_add_stream to identify the stream
          to remove.
 * Returns:
 *    0 on success negative error code on failure (from errno.h).
 */
int cras_client_rm_stream(struct cras_client* client,
                          cras_stream_id_t stream_id);

/* Sets the volume scaling factor for the given stream.
 *
 * Requires execution of cras_client_run_thread().
 *
 * Args:
 *    client - Client owning the stream.
 *    stream_id - ID returned from cras_client_add_stream.
 *    volume_scaler - 0.0-1.0 the new value to scale this stream by.
 */
int cras_client_set_stream_volume(struct cras_client* client,
                                  cras_stream_id_t stream_id,
                                  float volume_scaler);

/* Sets an output device to be the echo reference of an input stream.
 * The output device is specified by the index. Before this call,
 * input streams requesting AEC effect would use the default echo
 * reference selected by system.
 *
 * When |dev_idx| is set with a value other than NO_DEVICE from
 * enum CRAS_SPECIAL_DEVICE, it means the client stream requests to
 * stick at using |dev_idx| no matter how the system default changes.
 * When |dev_idx| is set to NO_DEVICE, it means the client requests
 * to use the system default.
 *
 * Client caller is responsible for monitoring the devices states
 * in server side and updating aec ref accordingly with valid
 * |dev_idx| values. Server side implementation will follow below
 * principles:
 * (a) If |dev_idx| is not found on server side, this will be a no-op.
 * (b) When device |dev_idx| is removed at any time, server side will
 * fallback to use the system default aec ref.
 *
 * Args:
 *    client - Client owning the stream.
 *    stream_id - ID returned from cras_client_add_stream.
 *    dev_idx - ID of the audio device to set as echo reference for
 *        given stream.
 * Returns:
 *    0 on success, negative error code on failure.
 */
int cras_client_set_aec_ref(struct cras_client* client,
                            cras_stream_id_t stream_id,
                            uint32_t dev_idx);
/*
 * System level functions.
 */

/* Sets the volume of the system.
 *
 * Volume here ranges from 0 to 100, and will be translated to dB based on the
 * output-specific volume curve.
 *
 * Args:
 *    client - The client from cras_client_create.
 *    volume - 0-100 the new volume index.
 * Returns:
 *    0 for success, -EPIPE if there is an I/O error talking to the server, or
 *    -EINVAL if 'client' is invalid.
 */
int cras_client_set_system_volume(struct cras_client* client, size_t volume);

/* Sets the mute state of the system.
 *
 * Args:
 *    client - The client from cras_client_create.
 *    mute - 0 is un-mute, 1 is muted.
 * Returns:
 *    0 for success, -EPIPE if there is an I/O error talking to the server, or
 *    -EINVAL if 'client' is invalid.
 */
int cras_client_set_system_mute(struct cras_client* client, int mute);

/* Sets the user mute state of the system.
 *
 * This is used for mutes caused by user interaction. Like the mute key.
 *
 * Args:
 *    client - The client from cras_client_create.
 *    mute - 0 is un-mute, 1 is muted.
 * Returns:
 *    0 for success, -EPIPE if there is an I/O error talking to the server, or
 *    -EINVAL if 'client' is invalid.
 */
int cras_client_set_user_mute(struct cras_client* client, int mute);

/* Sets the mute locked state of the system.
 *
 * Changing mute state is impossible when this flag is set to locked.
 *
 * Args:
 *    client - The client from cras_client_create.
 *    locked - 0 is un-locked, 1 is locked.
 * Returns:
 *    0 for success, -EPIPE if there is an I/O error talking to the server, or
 *    -EINVAL if 'client' is invalid.
 */
int cras_client_set_system_mute_locked(struct cras_client* client, int locked);

/* Sets the capture mute state of the system.
 *
 * Recordings will be muted when this is set.
 *
 * Args:
 *    client - The client from cras_client_create.
 *    mute - 0 is un-mute, 1 is muted.
 * Returns:
 *    0 for success, -EPIPE if there is an I/O error talking to the server, or
 *    -EINVAL if 'client' is invalid.
 */
int cras_client_set_system_capture_mute(struct cras_client* client, int mute);

/* Sets the capture mute locked state of the system.
 *
 * Changing mute state is impossible when this flag is set to locked.
 *
 * Args:
 *    client - The client from cras_client_create.
 *    locked - 0 is un-locked, 1 is locked.
 * Returns:
 *    0 for success, -EPIPE if there is an I/O error talking to the server, or
 *    -EINVAL if 'client' is invalid.
 */
int cras_client_set_system_capture_mute_locked(struct cras_client* client,
                                               int locked);

/* Gets the current system volume.
 *
 * Requires that the connection to the server has been established.
 *
 * Args:
 *    client - The client from cras_client_create.
 * Returns:
 *    The current system volume between 0 and 100.
 */
size_t cras_client_get_system_volume(const struct cras_client* client);

/* Gets the current system mute state.
 *
 * Requires that the connection to the server has been established.
 *
 * Args:
 *    client - The client from cras_client_create.
 * Returns:
 *    0 if not muted, 1 if it is.
 */
int cras_client_get_system_muted(const struct cras_client* client);

/* Gets the current user mute state.
 *
 * Requires that the connection to the server has been established.
 *
 * Args:
 *    client - The client from cras_client_create.
 * Returns:
 *    0 if not muted, 1 if it is.
 */
int cras_client_get_user_muted(const struct cras_client* client);

/* Gets the current system capture mute state.
 *
 * Requires that the connection to the server has been established.
 *
 * Args:
 *    client - The client from cras_client_create.
 * Returns:
 *    0 if capture is not muted, 1 if it is.
 */
int cras_client_get_system_capture_muted(const struct cras_client* client);

/* Gets the current minimum system volume.
 * Args:
 *    client - The client from cras_client_create.
 * Returns:
 *    The minimum value for the current output device in dBFS * 100.  This is
 *    the level of attenuation at volume == 1.
 */
long cras_client_get_system_min_volume(const struct cras_client* client);

/* Gets the current maximum system volume.
 * Args:
 *    client - The client from cras_client_create.
 * Returns:
 *    The maximum value for the current output device in dBFS * 100.  This is
 *    the level of attenuation at volume == 100.
 */
long cras_client_get_system_max_volume(const struct cras_client* client);

/* Gets the default output buffer size.
 * Args:
 *    client - The client from cras_client_create.
 * Returns:
 *    Default output buffer size in frames. A negative error on failure.
 */
int cras_client_get_default_output_buffer_size(struct cras_client* client);

/* Gets audio debug info.
 *
 * Requires that the connection to the server has been established.
 * Access to the resulting pointer is not thread-safe.
 *
 * Args:
 *    client - The client from cras_client_create.
 * Returns:
 *    A pointer to the debug info.  This info is only updated when requested by
 *    calling cras_client_update_audio_debug_info.
 */
const struct audio_debug_info* cras_client_get_audio_debug_info(
    const struct cras_client* client);

/* Gets bluetooth debug info.
 *
 * Requires that the connection to the server has been established.
 *
 * Args:
 *    client - The client from cras_client_create.
 * Returns:
 *    A pointer to the debug info. This info is updated and requested by
 *    calling cras_client_update_bt_debug_info.
 */
const struct cras_bt_debug_info* cras_client_get_bt_debug_info(
    const struct cras_client* client);

/* Gets main thread debug info.
 * Args:
 *    client - The client from cras_client_create.
 * Returns:
 *    A pointer to the debug info. This info is updated and requested by
 *    calling cras_client_update_main_thread_debug_info.
 */
const struct main_thread_debug_info* cras_client_get_main_thread_debug_info(
    const struct cras_client* client);

/* Gets audio thread snapshot buffer.
 *
 * Requires that the connection to the server has been established.
 * Access to the resulting pointer is not thread-safe.
 *
 * Args:
 *    client - The client from cras_client_create.
 * Returns:
 *    A pointer to the snapshot buffer.  This info is only updated when
 *    requested by calling cras_client_update_audio_thread_snapshots.
 */
const struct cras_audio_thread_snapshot_buffer*
cras_client_get_audio_thread_snapshot_buffer(const struct cras_client* client);

/* Gets the number of streams currently attached to the server.
 *
 * This is the total number of capture and playback streams. If the ts argument
 * is not null, then it will be filled with the last time audio was played or
 * recorded. ts will be set to the current time if streams are currently
 * active.
 *
 * Requires that the connection to the server has been established.
 *
 * Args:
 *    client - The client from cras_client_create.
 *    ts - Filled with the timestamp of the last stream.
 * Returns:
 *    The number of active streams.
 */
unsigned cras_client_get_num_active_streams(const struct cras_client* client,
                                            struct timespec* ts);

/*
 * Utility functions.
 */

/* Returns the number of bytes in an audio frame for a stream.
 * Args:
 *    format - The format of the audio stream.  Specifies bits per sample,
 *        number of channels, and sample rate.
 * Returns:
 *   Positive number of bytes in a frame, or a negative error code if fmt is
 *   NULL.
 */
int cras_client_format_bytes_per_frame(struct cras_audio_format* fmt);

/* For playback streams, calculates the latency of the next sample written.
 * Only valid when called from the audio callback function for the stream
 * (aud_cb).
 * Args:
 *    sample_time - The sample time stamp passed in to aud_cb.
 *    delay - Out parameter will be filled with the latency.
 * Returns:
 *    0 on success, -EINVAL if delay is NULL.
 */
int cras_client_calc_playback_latency(const struct timespec* sample_time,
                                      struct timespec* delay);

/* For capture returns the latency of the next frame to be read from the buffer
 * (based on when it was captured).  Only valid when called from the audio
 * callback function for the stream (aud_cb).
 * Args:
 *    sample_time - The sample time stamp passed in to aud_cb.
 *    delay - Out parameter will be filled with the latency.
 * Returns:
 *    0 on success, -EINVAL if delay is NULL.
 */
int cras_client_calc_capture_latency(const struct timespec* sample_time,
                                     struct timespec* delay);

/* Set the volume of the given output node. Only for output nodes.
 *
 * Args:
 *    client - The client from cras_client_create.
 *    node_id - ID of the node.
 *    volume - New value for node volume.
 */
int cras_client_set_node_volume(struct cras_client* client,
                                cras_node_id_t node_id,
                                uint8_t volume);

/* Swap the left and right channel of the given node.
 *
 * Args:
 *    client - The client from cras_client_create.
 *    node_id - ID of the node.
 *    enable - 1 to enable swap mode, 0 to disable.
 */
int cras_client_swap_node_left_right(struct cras_client* client,
                                     cras_node_id_t node_id,
                                     int enable);

/* Set the capture gain of the given input node.  Only for input nodes.
 *
 * Args:
 *    client - The client from cras_client_create.
 *    node_id - ID of the node.
 *    gain - New capture gain for the node, in range (0, 100) which will
 *        linearly maps to [0, 50) to [-2000, 0) and [50, 100] to [0, max_gain]
 *        100*dBFS. If it is an internal mic, it will query
 *        max_internal_mic_gain from board.ini instead of using the default
 *        value 2000.
 */
int cras_client_set_node_capture_gain(struct cras_client* client,
                                      cras_node_id_t node_id,
                                      long gain);

/* Add a test iodev to the iodev list.
 *
 * Args:
 *    client - The client from cras_client_create.
 *    type - The type of test iodev, see cras_types.h
 */
int cras_client_add_test_iodev(struct cras_client* client,
                               enum TEST_IODEV_TYPE type);

/* Finds the first node of the given type.
 *
 * This is used for finding a special hotword node.
 *
 * Requires that the connection to the server has been established.
 *
 * Args:
 *    client - The client from cras_client_create.
 *    type - The type of device to find.
 *    direction - Search input or output devices.
 *    node_id - The found node on success.
 * Returns:
 *    0 on success, a negative error on failure.
 */
int cras_client_get_first_node_type_idx(const struct cras_client* client,
                                        enum CRAS_NODE_TYPE type,
                                        enum CRAS_STREAM_DIRECTION direction,
                                        cras_node_id_t* node_id);

/* Finds the first device that contains a node of the given type.
 *
 * This is used for finding a special hotword device.
 *
 * Requires that the connection to the server has been established.
 *
 * Args:
 *    client - The client from cras_client_create.
 *    type - The type of device to find.
 *    direction - Search input or output devices.
 * Returns the device index of a negative error on failure.
 */
int cras_client_get_first_dev_type_idx(const struct cras_client* client,
                                       enum CRAS_NODE_TYPE type,
                                       enum CRAS_STREAM_DIRECTION direction);

/* Sets the suspend state of audio playback and capture.
 *
 * Set this before putting the system into suspend.
 *
 * Args:
 *    client - The client from cras_client_create.
 *    suspend - Suspend the system if non-zero, otherwise resume.
 */
int cras_client_set_suspend(struct cras_client* client, int suspend);

/* Gets the set of supported hotword language models on a node. The supported
 * models may differ on different nodes.
 *
 * Args:
 *    client - The client from cras_client_create.
 *    node_id - ID of a hotword input node (CRAS_NODE_TYPE_HOTWORD).
 *    cb - The function to be called when hotword models are ready.
 * Returns:
 *    0 on success.
 */
int cras_client_get_hotword_models(struct cras_client* client,
                                   cras_node_id_t node_id,
                                   get_hotword_models_cb_t cb);

/* Sets the hotword language model on a node. If there are existing streams on
 * the hotword input node when this function is called, they need to be closed
 * then re-opend for the model change to take effect.
 * Args:
 *    client - The client from cras_client_create.
 *    node_id - ID of a hotword input node (CRAS_NODE_TYPE_HOTWORD).
 *    model_name - Name of the model to use, e.g. "en_us".
 * Returns:
 *    0 on success.
 *    -EINVAL if client or node_id is invalid.
 *    -ENOENT if the specified model is not found.
 */
int cras_client_set_hotword_model(struct cras_client* client,
                                  cras_node_id_t node_id,
                                  const char* model_name);

/*
 * Creates a hotword stream and waits for the hotword to trigger.
 *
 * Args:
 *    client - The client to add the stream to (from cras_client_create).
 *    user_data - Pointer that will be passed to the callback.
 *    trigger_cb - Called when a hotword is triggered.
 *    err_cb - Called when there is an error with the stream.
 *    handle_out - On success will be filled with a cras_hotword_handle.
 * Returns:
 *    0 on success, negative error code on failure (from errno.h).
 */
int cras_client_enable_hotword_callback(
    struct cras_client* client,
    void* user_data,
    cras_hotword_trigger_cb_t trigger_cb,
    cras_hotword_error_cb_t err_cb,
    struct cras_hotword_handle** handle_out);

/*
 * Closes a hotword stream that was created by cras_client_wait_for_hotword.
 *
 * Args:
 *    client - Client to remove the stream (returned from cras_client_create).
 *    handle - cras_hotword_handle returned from cras_client_wait_for_hotword.
 * Returns:
 *    0 on success negative error code on failure (from errno.h).
 */
int cras_client_disable_hotword_callback(struct cras_client* client,
                                         struct cras_hotword_handle* handle);

/* Starts or stops the aec dump task on server side.
 * Args:
 *    client - The client from cras_client_create.
 *    stream_id - The id of the input stream running with aec effect.
 *    start - True to start APM debugging, otherwise to stop it.
 *    fd - File descriptor of the file to store aec dump result.
 */
int cras_client_set_aec_dump(struct cras_client* client,
                             cras_stream_id_t stream_id,
                             int start,
                             int fd);
/*
 * Reloads the aec.ini config file on server side.
 */
int cras_client_reload_aec_config(struct cras_client* client);

/*
 * Returns if AEC is supported.
 */
int cras_client_get_aec_supported(struct cras_client* client);

/*
 * Returns the AEC group ID if available.
 */
int cras_client_get_aec_group_id(struct cras_client* client);

/*
 * Sets the flag to enable bluetooth wideband speech in server.
 */
int cras_client_set_bt_wbs_enabled(struct cras_client* client, bool enabled);

/* Set the context pointer for system state change callbacks.
 * Args:
 *    client - The client from cras_client_create.
 *    context - The context pointer passed to all callbacks.
 */
void cras_client_set_state_change_callback_context(struct cras_client* client,
                                                   void* context);

/* Requests the device ID of the flexible loopback of the given client types
 * mask. A floop will be created if it does not already exist, otherwise an
 * existing one will be returned.
 *
 * See struct cras_floop_params for the meaning of client_types_mask.
 */
int32_t cras_client_get_floop_dev_idx_by_client_types(
    struct cras_client* client,
    int64_t client_types_mask);

/* Output volume change callback.
 *
 * Args:
 *    context - Context pointer set with
 *              cras_client_set_state_change_callback_context().
 *    volume - The system output volume, ranging from 0 to 100.
 */
typedef void (*cras_client_output_volume_changed_callback)(void* context,
                                                           int32_t volume);

/* Output mute change callback.
 *
 * Args:
 *    context - Context pointer set with
 *              cras_client_set_state_change_callback_context().
 *    muted - Non-zero when the audio is muted, zero otherwise.
 *    user_muted - Non-zero when the audio has been muted by the
 *                 user, zero otherwise.
 *    mute_locked - Non-zero when the mute funcion is locked,
 *                  zero otherwise.
 */
typedef void (*cras_client_output_mute_changed_callback)(void* context,
                                                         int muted,
                                                         int user_muted,
                                                         int mute_locked);

/* Capture gain change callback.
 *
 * Args:
 *    context - Context pointer set with
 *              cras_client_set_state_change_callback_context().
 *    gain - The system capture gain, in centi-decibels.
 */
typedef void (*cras_client_capture_gain_changed_callback)(void* context,
                                                          int32_t gain);

/* Capture mute change callback.
 *
 * Args:
 *    context - Context pointer set with
 *              cras_client_set_state_change_callback_context().
 *    muted - Non-zero when the audio is muted, zero otherwise.
 *    mute_locked - Non-zero when the mute funcion is locked,
 *                  zero otherwise.
 */
typedef void (*cras_client_capture_mute_changed_callback)(void* context,
                                                          int muted,
                                                          int mute_locked);

/* Nodes change callback.
 *
 * Args:
 *    context - Context pointer set with
 *              cras_client_set_state_change_callback_context().
 */
typedef void (*cras_client_nodes_changed_callback)(void* context);

/* Active node change callback.
 *
 * Args:
 *    context - Context pointer set with
 *              cras_client_set_state_change_callback_context().
 *    direction - Indicates the direction of the selected node.
 *    node_id - The ID of the selected node. Special device ID values
 *              defined by CRAS_SPECIAL_DEVICE will be used when no other
 *              device or node is selected or between selections.
 */
typedef void (*cras_client_active_node_changed_callback)(
    void* context,
    enum CRAS_STREAM_DIRECTION direction,
    cras_node_id_t node_id);

/* Output node volume change callback.
 *
 * Args:
 *    context - Context pointer set with
 *              cras_client_set_state_change_callback_context().
 *    node_id - The ID of the output node.
 *    volume - The volume for this node with range 0 to 100.
 */
typedef void (*cras_client_output_node_volume_changed_callback)(
    void* context,
    cras_node_id_t node_id,
    int32_t volume);

/* Node left right swapped change callback.
 *
 * Args:
 *    context - Context pointer set with
 *              cras_client_set_state_change_callback_context().
 *    node_id - The ID of the node.
 *    swapped - Non-zero if the node is left-right swapped, zero otherwise.
 */
typedef void (*cras_client_node_left_right_swapped_changed_callback)(
    void* context,
    cras_node_id_t node_id,
    int swapped);

/* Input node gain change callback.
 * Args:
 *    context - Context pointer set with
 *              cras_client_set_state_change_callback_context().
 *    node_id - The ID of the input node.
 *    gain - The gain for this node in centi-decibels.
 */
typedef void (*cras_client_input_node_gain_changed_callback)(
    void* context,
    cras_node_id_t node_id,
    int32_t gain);

/* Number of active streams change callback.
 *
 * Args:
 *    context - Context pointer set with
 *              cras_client_set_state_change_callback_context().
 *    direction - Indicates the direction of the stream's node.
 *    num_active_streams - The number of active streams.
 */
typedef void (*cras_client_num_active_streams_changed_callback)(
    void* context,
    enum CRAS_STREAM_DIRECTION direction,
    uint32_t num_active_streams);

/* Set system state information callbacks.
 * NOTE: These callbacks are executed from the client control thread.
 * Each state change callback is given the context pointer set with
 * cras_client_set_state_change_callback_context(). The context pointer is
 * NULL by default.
 * Args:
 *    client - The client from cras_client_create.
 *    cb - The callback, or NULL to disable the call-back.
 * Returns:
 *    0 for success or negative errno error code on error.
 */
int cras_client_set_output_volume_changed_callback(
    struct cras_client* client,
    cras_client_output_volume_changed_callback cb);
int cras_client_set_output_mute_changed_callback(
    struct cras_client* client,
    cras_client_output_mute_changed_callback cb);
int cras_client_set_capture_gain_changed_callback(
    struct cras_client* client,
    cras_client_capture_gain_changed_callback cb);
int cras_client_set_capture_mute_changed_callback(
    struct cras_client* client,
    cras_client_capture_mute_changed_callback cb);
int cras_client_set_nodes_changed_callback(
    struct cras_client* client,
    cras_client_nodes_changed_callback cb);
int cras_client_set_active_node_changed_callback(
    struct cras_client* client,
    cras_client_active_node_changed_callback cb);
int cras_client_set_output_node_volume_changed_callback(
    struct cras_client* client,
    cras_client_output_node_volume_changed_callback cb);
int cras_client_set_node_left_right_swapped_changed_callback(
    struct cras_client* client,
    cras_client_node_left_right_swapped_changed_callback cb);
int cras_client_set_input_node_gain_changed_callback(
    struct cras_client* client,
    cras_client_input_node_gain_changed_callback cb);
int cras_client_set_num_active_streams_changed_callback(
    struct cras_client* client,
    cras_client_num_active_streams_changed_callback cb);

/*
 * The functions below prefixed with libcras wrap the original CRAS library
 * They provide an interface that maps the pointers to the functions above.
 * Please add a new function instead of modifying the existing function.
 * Here are some rules about how to add a new function:
 * 1. Increase the CRAS_API_VERSION by 1.
 * 2. Write a new function in cras_client.c.
 * 3. Append the corresponding pointer to the structure. Remember DO NOT change
 *    the order of functions in the structs.
 * 4. Assign the pointer to the new function in cras_client.c.
 * 5. Create the inline function in cras_client.h, which is used by clients.
 *    Remember to add DISABLE_CFI_ICALL on the inline function.
 * 6. Add CHECK_VERSION in the inline function. If the api_version is smaller
 *    than the supported version, this inline function will return -ENOSYS.
 */

#define CRAS_API_VERSION 10
#define CHECK_VERSION(object, version) \
  if (object->api_version < version) { \
    return -ENOSYS;                    \
  }

/*
 * The inline functions use the indirect function call. Therefore, they are
 * incompatible with CFI-icall.
 */
#if defined(__clang__)
#define DISABLE_CFI_ICALL __attribute__((no_sanitize("cfi-icall")))
#else
#define DISABLE_CFI_ICALL
#endif

struct libcras_node_info {
  int api_version;
  struct cras_node_info* node_;
  int (*get_id)(struct cras_node_info* node, uint64_t* id);
  int (*get_dev_idx)(struct cras_node_info* node, uint32_t* dev_idx);
  int (*get_node_idx)(struct cras_node_info* node, uint32_t* node_idx);
  int (*get_max_supported_channels)(struct cras_node_info* node,
                                    uint32_t* max_supported_channels);
  int (*is_plugged)(struct cras_node_info* node, bool* plugged);
  int (*is_active)(struct cras_node_info* node, bool* active);
  int (*get_type)(struct cras_node_info* node, char** name);
  int (*get_node_name)(struct cras_node_info* node, char** name);
  int (*get_dev_name)(struct cras_node_info* node, char** name);
};

struct libcras_client {
  int api_version;
  struct cras_client* client_;
  int (*connect)(struct cras_client* client);
  int (*connect_timeout)(struct cras_client* client, unsigned int timeout_ms);
  int (*connected_wait)(struct cras_client* client);
  int (*run_thread)(struct cras_client* client);
  int (*stop)(struct cras_client* client);
  int (*add_pinned_stream)(struct cras_client* client,
                           uint32_t dev_idx,
                           cras_stream_id_t* stream_id_out,
                           struct cras_stream_params* config);
  int (*rm_stream)(struct cras_client* client, cras_stream_id_t stream_id);
  int (*set_stream_volume)(struct cras_client* client,
                           cras_stream_id_t stream_id,
                           float volume_scaler);
  int (*get_nodes)(struct cras_client* client,
                   enum CRAS_STREAM_DIRECTION direction,
                   struct libcras_node_info*** nodes,
                   size_t* num);
  int (*get_default_output_buffer_size)(struct cras_client* client, int* size);
  int (*get_aec_group_id)(struct cras_client* client, int* id);
  int (*get_aec_supported)(struct cras_client* client, int* supported);
  int (*get_system_muted)(struct cras_client* client, int* muted);
  int (*set_system_mute)(struct cras_client* client, int mute);
  int (*get_loopback_dev_idx)(struct cras_client* client, int* idx);
  int (*set_aec_ref)(struct cras_client* client,
                     cras_stream_id_t stream_id,
                     uint32_t dev_idx);
  int (*get_floop_dev_idx_by_client_types)(struct cras_client* client,
                                           int64_t client_types_mask);
  int (*get_system_capture_muted)(struct cras_client* client, int* muted);
  int (*set_aec_dump)(struct cras_client* client,
                      cras_stream_id_t stream_id,
                      int start,
                      int fd);
  int (*get_agc_supported)(struct cras_client* client, int* supported);
  int (*get_ns_supported)(struct cras_client* client, int* supported);
  int (*set_client_type)(struct cras_client* client,
                         enum CRAS_CLIENT_TYPE client_type);
};

struct cras_stream_cb_data;
struct libcras_stream_cb_data {
  int api_version;
  struct cras_stream_cb_data* data_;
  int (*get_stream_id)(struct cras_stream_cb_data* data, cras_stream_id_t* id);
  int (*get_buf)(struct cras_stream_cb_data* data, uint8_t** buf);
  int (*get_frames)(struct cras_stream_cb_data* data, unsigned int* frames);
  int (*get_latency)(struct cras_stream_cb_data* data,
                     struct timespec* latency);
  int (*get_user_arg)(struct cras_stream_cb_data* data, void** user_arg);
  int (*get_overrun_frames)(struct cras_stream_cb_data* data,
                            unsigned int* frames);
  int (*get_dropped_samples_duration)(struct cras_stream_cb_data* data,
                                      struct timespec* duration);
  int (*get_underrun_duration)(struct cras_stream_cb_data* data,
                               struct timespec* duration);
};
typedef int (*libcras_stream_cb_t)(struct libcras_stream_cb_data* data);

struct libcras_stream_params {
  int api_version;
  struct cras_stream_params* params_;
  int (*set)(struct cras_stream_params* params,
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
             size_t num_channels);
  int (*set_channel_layout)(struct cras_stream_params* params,
                            int length,
                            const int8_t* layout);
  void (*enable_aec)(struct cras_stream_params* params);
  void (*enable_ns)(struct cras_stream_params* params);
  void (*enable_agc)(struct cras_stream_params* params);
  void (*allow_aec_on_dsp)(struct cras_stream_params* params);
  void (*allow_ns_on_dsp)(struct cras_stream_params* params);
  void (*allow_agc_on_dsp)(struct cras_stream_params* params);
  void (*enable_ignore_ui_gains)(struct cras_stream_params* params);
};

/*
 * Creates a new client.
 * Returns:
 *    If success, return a valid libcras_client pointer. Otherwise, return
 *    NULL.
 */
struct libcras_client* libcras_client_create();

/*
 * Destroys a client.
 * Args:
 *    client - pointer returned from "libcras_client_create".
 */
void libcras_client_destroy(struct libcras_client* client);

/*
 * Connects a client to the running server.
 * Waits forever (until interrupted or connected).
 * Args:
 *    client - pointer returned from "libcras_client_create".
 * Returns:
 *    0 on success, or a negative error code on failure (from errno.h).
 */
DISABLE_CFI_ICALL
inline int libcras_client_connect(struct libcras_client* client) {
  return client->connect(client->client_);
}

/*
 * Connects a client to the running server, retries until timeout.
 * Args:
 *    client - pointer returned from "libcras_client_create".
 *    timeout_ms - timeout in milliseconds or negative to wait forever.
 * Returns:
 *    0 on success, or a negative error code on failure (from errno.h).
 */
DISABLE_CFI_ICALL
inline int libcras_client_connect_timeout(struct libcras_client* client,
                                          unsigned int timeout_ms) {
  return client->connect_timeout(client->client_, timeout_ms);
}

/*
 * Wait up to 1 second for the client thread to complete the server connection.
 *
 * After libcras_client_run_thread() is executed, this function can be
 * used to ensure that the connection has been established with the server and
 * ensure that any information about the server is up to date. If
 * libcras_client_run_thread() has not yet been executed, or
 * libcras_client_stop() was executed and thread isn't running, then this
 * function returns -EINVAL.
 *
 * Args:
 *    client - pointer returned from "libcras_client_create".
 * Returns:
 *    0 on success, or a negative error code on failure (from errno.h).
 */
DISABLE_CFI_ICALL
inline int libcras_client_connected_wait(struct libcras_client* client) {
  return client->connected_wait(client->client_);
}

/*
 * Begins running the client control thread.
 *
 * Required for stream operations and other operations noted below.
 *
 * Args:
 *    client - pointer returned from "libcras_client_create".
 * Returns:
 *    0 on success, or a negative error code on failure (from errno.h).
 */
DISABLE_CFI_ICALL
inline int libcras_client_run_thread(struct libcras_client* client) {
  return client->run_thread(client->client_);
}

/*
 * Stops running a client.
 * This function is executed automatically by cras_client_destroy().
 * Args:
 *    client - pointer returned from "libcras_client_create".
 * Returns:
 *    0 on success or if the thread was already stopped, -EINVAL if the client
 *    isn't valid.
 */
DISABLE_CFI_ICALL
inline int libcras_client_stop(struct libcras_client* client) {
  return client->stop(client->client_);
}

/*
 * Creates a pinned stream and return the stream id or < 0 on error.
 *
 * Requires execution of libcras_client_run_thread(), and an active
 * connection to the audio server.
 *
 * Args:
 *    client - pointer returned from "libcras_client_create".
 *    dev_idx - Index of the device to attach the newly created stream.
 *    stream_id_out - On success will be filled with the new stream id.
 *        Guaranteed to be set before any callbacks are made.
 *    params - The pointer specifying the parameters for the stream.
 *        (returned from libcras_stream_params_create)
 * Returns:
 *    0 on success, negative error code on failure (from errno.h).
 */
DISABLE_CFI_ICALL
inline int libcras_client_add_pinned_stream(
    struct libcras_client* client,
    uint32_t dev_idx,
    cras_stream_id_t* stream_id_out,
    struct libcras_stream_params* params) {
  return client->add_pinned_stream(client->client_, dev_idx, stream_id_out,
                                   params->params_);
}

/*
 * Removes a currently playing/capturing stream.
 *
 * Requires execution of libcras_client_run_thread().
 *
 * Args:
 *    client - pointer returned from "libcras_client_create".
 *    stream_id - ID returned from libcras_client_add_stream to identify
 *        the stream to remove.
 * Returns:
 *    0 on success negative error code on failure (from errno.h).
 */
DISABLE_CFI_ICALL
inline int libcras_client_rm_stream(struct libcras_client* client,
                                    cras_stream_id_t stream_id) {
  return client->rm_stream(client->client_, stream_id);
}

/* Sets an output device to be the echo reference of an input stream.
 * The output device is specified by the index. Before this call,
 * input streams requesting AEC effect would use the default echo
 * reference selected by system.
 *
 * See cras_client_set_aec_ref for more explanation.
 *
 * Args:
 *    client - Client owning the stream.
 *    stream_id - ID returned from cras_client_add_stream.
 *    dev_idx - ID of the audio device to set as echo reference for
 *        given stream.
 * Returns:
 *    0 on success, negative error code on failure.
 */
DISABLE_CFI_ICALL
inline int libcras_client_set_aec_ref(struct libcras_client* client,
                                      cras_stream_id_t stream_id,
                                      uint32_t dev_idx) {
  CHECK_VERSION(client, 3);
  return client->set_aec_ref(client->client_, stream_id, dev_idx);
}

/*
 * Sets the volume scaling factor for the given stream.
 *
 * Requires execution of cras_client_run_thread().
 *
 * Args:
 *    client - pointer returned from "libcras_client_create".
 *    stream_id - ID returned from libcras_client_add_stream.
 *    volume_scaler - 0.0-1.0 the new value to scale this stream by.
 * Returns:
 *    0 on success negative error code on failure (from errno.h).
 */
DISABLE_CFI_ICALL
inline int libcras_client_set_stream_volume(struct libcras_client* client,
                                            cras_stream_id_t stream_id,
                                            float volume_scaler) {
  return client->set_stream_volume(client->client_, stream_id, volume_scaler);
}

/*
 * Gets the current list of audio nodes.
 *
 * Args:
 *    client - Pointer returned from "libcras_client_create".
 *    direction - Input or output.
 *    nodes - Array that will be filled with libcras_node_info pointers.
 *    num - Pointer to store the size of the array.
 * Returns:
 *    0 on success negative error code on failure (from errno.h).
 *    Remember to call libcras_node_info_array_destroy to free the array.
 */
DISABLE_CFI_ICALL
inline int libcras_client_get_nodes(struct libcras_client* client,
                                    enum CRAS_STREAM_DIRECTION direction,
                                    struct libcras_node_info*** nodes,
                                    size_t* num) {
  return client->get_nodes(client->client_, direction, nodes, num);
}

/*
 * Gets the default output buffer size.
 * Args:
 *    client - Pointer returned from "libcras_client_create".
 *    size - The pointer to save the result.
 * Returns:
 *    0 on success negative error code on failure (from errno.h).
 */
DISABLE_CFI_ICALL
inline int libcras_client_get_default_output_buffer_size(
    struct libcras_client* client,
    int* size) {
  return client->get_default_output_buffer_size(client->client_, size);
}

/*
 * Gets the AEC group ID.
 * Args:
 *    client - Pointer returned from "libcras_client_create".
 *    id - The pointer to save the result.
 * Returns:
 *    0 on success negative error code on failure (from errno.h).
 */
DISABLE_CFI_ICALL
inline int libcras_client_get_aec_group_id(struct libcras_client* client,
                                           int* id) {
  return client->get_aec_group_id(client->client_, id);
}

/*
 * Gets whether AGC is supported.
 * Args:
 *    client - Pointer returned from "libcras_client_create".
 *    supported - The pointer to save the result.
 * Returns:
 *    0 on success negative error code on failure (from errno.h).
 */
DISABLE_CFI_ICALL
inline int libcras_client_get_agc_supported(struct libcras_client* client,
                                            int* supported) {
  CHECK_VERSION(client, 7);
  return client->get_agc_supported(client->client_, supported);
}

/*
 * Gets whether NS is supported.
 * Args:
 *    client - Pointer returned from "libcras_client_create".
 *    supported - The pointer to save the result.
 * Returns:
 *    0 on success negative error code on failure (from errno.h).
 */
DISABLE_CFI_ICALL
inline int libcras_client_get_ns_supported(struct libcras_client* client,
                                           int* supported) {
  CHECK_VERSION(client, 7);
  return client->get_ns_supported(client->client_, supported);
}

/*
 * Gets whether AEC is supported.
 * Args:
 *    client - Pointer returned from "libcras_client_create".
 *    supported - The pointer to save the result.
 * Returns:
 *    0 on success negative error code on failure (from errno.h).
 */
DISABLE_CFI_ICALL
inline int libcras_client_get_aec_supported(struct libcras_client* client,
                                            int* supported) {
  return client->get_aec_supported(client->client_, supported);
}

/*
 * Gets whether the system is muted.
 * Args:
 *    client - Pointer returned from "libcras_client_create".
 *    muted - The pointer to save the result.
 * Returns:
 *    0 on success negative error code on failure (from errno.h).
 */
DISABLE_CFI_ICALL
inline int libcras_client_get_system_muted(struct libcras_client* client,
                                           int* muted) {
  return client->get_system_muted(client->client_, muted);
}

/*
 * Gets whether the system capture is muted.
 * Args:
 *    client - Pointer returned from "libcras_client_create".
 *    muted - The pointer to save the result.
 * Returns:
 *    0 on success negative error code on failure (from errno.h).
 */
DISABLE_CFI_ICALL
inline int libcras_client_get_system_capture_muted(
    struct libcras_client* client,
    int* muted) {
  CHECK_VERSION(client, 5);
  return client->get_system_capture_muted(client->client_, muted);
}

/*
 * Starts or stops the aec dump task on server side.
 * Args:
 *    client - The client from cras_client_create.
 *    stream_id - The id of the input stream running with aec effect.
 *    start - True to start APM debugging, otherwise to stop it.
 *    fd - File descriptor of the file to store aec dump result.
 * Returns:
 *    0 on success negative error code on failure (from errno.h).
 */
DISABLE_CFI_ICALL
inline int libcras_client_set_aec_dump(struct libcras_client* client,
                                       cras_stream_id_t stream_id,
                                       int start,
                                       int fd) {
  CHECK_VERSION(client, 6);
  return client->set_aec_dump(client->client_, stream_id, start, fd);
}

/*
 * Mutes or unmutes the system.
 * Args:
 *    client - Pointer returned from "libcras_client_create".
 *    mute - 1 is to mute and 0 is to unmute.
 * Returns:
 *    0 on success negative error code on failure (from errno.h).
 */
DISABLE_CFI_ICALL
inline int libcras_client_set_system_mute(struct libcras_client* client,
                                          int mute) {
  return client->set_system_mute(client->client_, mute);
}

/*
 * Gets the index of the loopback device.
 * Args:
 *    client - Pointer returned from "libcras_client_create".
 *    idx - The pointer to save the result.
 * Returns:
 *    0 on success negative error code on failure (from errno.h).
 */
DISABLE_CFI_ICALL
inline int libcras_client_get_loopback_dev_idx(struct libcras_client* client,
                                               int* idx) {
  return client->get_loopback_dev_idx(client->client_, idx);
}

/*
 * Gets the index of the flexible loopback device.
 * Args:
 *    client - Pointer returned from "libcras_client_create".
 *    client_types_mask - Bitmask of CRAS_CLIENT_TYPE to loopback from.
 *    idx - The pointer to save the result.
 * Returns:
 *    The loopback device id on success;
 *    negative error code on failure (from errno.h).
 */
DISABLE_CFI_ICALL
inline int libcras_client_get_floop_dev_idx_by_client_types(
    struct libcras_client* client,
    int64_t client_types_mask) {
  CHECK_VERSION(client, 4);
  return client->get_floop_dev_idx_by_client_types(client->client_,
                                                   client_types_mask);
}

/*
 * Sets the client type on the given client.
 * Args:
 *    client - Pointer returned from "libcras_client_create"
 *    client_type - A client type.
 * Returns:
 *    0 on success, or negative error code on failure.
 */
DISABLE_CFI_ICALL
inline int libcras_client_set_client_type(struct libcras_client* client,
                                          enum CRAS_CLIENT_TYPE client_type) {
  CHECK_VERSION(client, 10);
  return client->set_client_type(client->client_, client_type);
}

/*
 * Creates a new struct to save stream params.
 * Returns:
 *    If success, return a valid libcras_stream_params pointer. Otherwise,
 *    return NULL.
 */
struct libcras_stream_params* libcras_stream_params_create();

/*
 * Destroys a stream params instance.
 * Args:
 *    params - The pointer returned from libcras_stream_params_create.
 */
void libcras_stream_params_destroy(struct libcras_stream_params* params);

/*
 * Setup stream configuration parameters.
 * Args:
 *    params - The pointer returned from libcras_stream_params_create.
 *    direction - Playback(CRAS_STREAM_OUTPUT) or capture(CRAS_STREAM_INPUT).
 *    buffer_frames - total number of audio frames to buffer (dictates latency).
 *    cb_threshold - For playback, call back for more data when the buffer
 *        reaches this level. For capture, this is ignored (Audio callback will
 *        be called when buffer_frames have been captured).
 *    stream_type - Media or talk (currently only support "default").
 *    client_type - The client type, like Chrome or CrOSVM.
 *    flags - Currently only used for CRAS_INPUT_STREAM_FLAG.
 *    user_data - Pointer that will be passed to the callback.
 *    stream_cb - The audio callback. Called when audio is needed(playback) or
 *        ready(capture).
 *    err_cb - Called when there is an error with the stream.
 *    rate - The sample rate of the audio stream.
 *    format - The format of the audio stream.
 *    num_channels - The number of channels of the audio stream.
 * Returns:
 *    0 on success negative error code on failure (from errno.h).
 */
DISABLE_CFI_ICALL
inline int libcras_stream_params_set(struct libcras_stream_params* params,
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
  return params->set(params->params_, direction, buffer_frames, cb_threshold,
                     stream_type, client_type, flags, user_data, stream_cb,
                     err_cb, rate, format, num_channels);
}

/*
 * Sets channel layout on given stream parameter.
 * Args:
 *    params - The pointer returned from libcras_stream_params_create.
 *    length - The length of the array.
 *    layout - An integer array representing the position of each channel in
 *    enum CRAS_CHANNEL.
 * Returns:
 *    0 on success negative error code on failure (from errno.h).
 */
DISABLE_CFI_ICALL
inline int libcras_stream_params_set_channel_layout(
    struct libcras_stream_params* params,
    int length,
    const int8_t* layout) {
  return params->set_channel_layout(params->params_, length, layout);
}

DISABLE_CFI_ICALL
inline int libcras_stream_params_allow_aec_on_dsp(
    struct libcras_stream_params* params) {
  CHECK_VERSION(params, 4);
  params->allow_aec_on_dsp(params->params_);
  return 0;
}

DISABLE_CFI_ICALL
inline int libcras_stream_params_allow_ns_on_dsp(
    struct libcras_stream_params* params) {
  CHECK_VERSION(params, 4);
  params->allow_ns_on_dsp(params->params_);
  return 0;
}

DISABLE_CFI_ICALL
inline int libcras_stream_params_allow_agc_on_dsp(
    struct libcras_stream_params* params) {
  CHECK_VERSION(params, 4);
  params->allow_agc_on_dsp(params->params_);
  return 0;
}

/*
 * Enables AEC on given stream parameter.
 * Args:
 *    params - The pointer returned from libcras_stream_params_create.
 * Returns:
 *    0 on success negative error code on failure (from errno.h).
 */
DISABLE_CFI_ICALL
inline int libcras_stream_params_enable_aec(
    struct libcras_stream_params* params) {
  params->enable_aec(params->params_);
  return 0;
}

/*
 * Enables NS on given stream parameter.
 * Args:
 *    params - The pointer returned from libcras_stream_params_create.
 * Returns:
 *    0 on success negative error code on failure (from errno.h).
 */
DISABLE_CFI_ICALL
inline int libcras_stream_params_enable_ns(
    struct libcras_stream_params* params) {
  CHECK_VERSION(params, 2);
  params->enable_ns(params->params_);
  return 0;
}

/*
 * Enables AGC on given stream parameter.
 * Args:
 *    params - The pointer returned from libcras_stream_params_create.
 * Returns:
 *    0 on success negative error code on failure (from errno.h).
 */
DISABLE_CFI_ICALL
inline int libcras_stream_params_enable_agc(
    struct libcras_stream_params* params) {
  CHECK_VERSION(params, 2);
  params->enable_agc(params->params_);
  return 0;
}

/*
 * Ignore Ui Gains on given stream parameter.
 * Args:
 *    params - The pointer returned from libcras_stream_params_create.
 * Returns:
 *    0 on success negative error code on failure (from errno.h).
 */
DISABLE_CFI_ICALL
inline int libcras_stream_params_ignore_ui_gains(
    struct libcras_stream_params* params) {
  CHECK_VERSION(params, 2);
  params->enable_ignore_ui_gains(params->params_);
  return 0;
}

/*
 * Gets stream id from the callback data.
 * Args:
 *    data - The pointer passed to the callback function.
 *    id - The pointer to save the stream id.
 * Returns:
 *    0 on success negative error code on failure (from errno.h).
 */
DISABLE_CFI_ICALL
inline int libcras_stream_cb_data_get_stream_id(
    struct libcras_stream_cb_data* data,
    cras_stream_id_t* id) {
  return data->get_stream_id(data->data_, id);
}

/*
 * Gets stream buf from the callback data.
 * Args:
 *    data - The pointer passed to the callback function.
 *    buf - The pointer to save the stream buffer.
 * Returns:
 *    0 on success negative error code on failure (from errno.h).
 */
DISABLE_CFI_ICALL
inline int libcras_stream_cb_data_get_buf(struct libcras_stream_cb_data* data,
                                          uint8_t** buf) {
  return data->get_buf(data->data_, buf);
}

/*
 * Gets how many frames to read or play from the callback data.
 * Args:
 *    data - The pointer passed to the callback function.
 *    frames - The pointer to save the number of frames.
 * Returns:
 *    0 on success negative error code on failure (from errno.h).
 */
DISABLE_CFI_ICALL
inline int libcras_stream_cb_data_get_frames(
    struct libcras_stream_cb_data* data,
    unsigned int* frames) {
  return data->get_frames(data->data_, frames);
}

/*
 * Gets the latency from the callback data.
 * Args:
 *    data - The pointer passed to the callback function.
 *    frames - The timespec pointer to save the latency.
 * Returns:
 *    0 on success negative error code on failure (from errno.h).
 */
DISABLE_CFI_ICALL
inline int libcras_stream_cb_data_get_latency(
    struct libcras_stream_cb_data* data,
    struct timespec* latency) {
  return data->get_latency(data->data_, latency);
}

/*
 * Gets the user data from the callback data.
 * Args:
 *    data - The pointer passed to the callback function.
 *    frames - The pointer to save the user data.
 * Returns:
 *    0 on success negative error code on failure (from errno.h).
 */
DISABLE_CFI_ICALL
inline int libcras_stream_cb_data_get_usr_arg(
    struct libcras_stream_cb_data* data,
    void** user_arg) {
  return data->get_user_arg(data->data_, user_arg);
}

/*
 * Gets the number of audio frames overwritten in the shared memory.
 * Args:
 *    data - The pointer passed to the callback function.
 *    frames - The pointer to save the number of frames.
 * Returns:
 *    0 on success negative error code on failure (from errno.h).
 */
DISABLE_CFI_ICALL
inline int libcras_stream_cb_data_get_overrun_frames(
    struct libcras_stream_cb_data* data,
    unsigned int* frames) {
  CHECK_VERSION(data, 8);
  return data->get_overrun_frames(data->data_, frames);
}

/*
 * Gets the duration of the dropped audio samples from hardware buffer.
 * Args:
 *    data - The pointer passed to the callback function.
 *    duration - The pointer to save the duration.
 * Returns:
 *    0 on success negative error code on failure (from errno.h).
 */
DISABLE_CFI_ICALL
inline int libcras_stream_cb_data_get_dropped_samples_duration(
    struct libcras_stream_cb_data* data,
    struct timespec* duration) {
  CHECK_VERSION(data, 8);
  return data->get_dropped_samples_duration(data->data_, duration);
}

/*
 * Gets the duration of the filled zero audio samples due to missing samples.
 * Args:
 *    data - The pointer passed to the callback function.
 *    frames - The pointer to save the number of frames.
 * Returns:
 *    0 on success negative error code on failure (from errno.h).
 */
DISABLE_CFI_ICALL
inline int libcras_stream_cb_data_get_underrun_duration(
    struct libcras_stream_cb_data* data,
    struct timespec* duration) {
  CHECK_VERSION(data, 9);
  return data->get_underrun_duration(data->data_, duration);
}

/*
 * Destroys a node info instance.
 * Args:
 *    node - The libcras_node_info pointer to destroy.
 */
void libcras_node_info_destroy(struct libcras_node_info* node);

/*
 * Destroys a node info array.
 * Args:
 *    nodes - The libcras_node_info pointer array to destroy.
 *    num - The size of the array.
 */
void libcras_node_info_array_destroy(struct libcras_node_info** nodes,
                                     size_t num);

/*
 * Gets ID from the node info pointer.
 * Args:
 *    node - The node info pointer. (Returned from libcras_client_get_nodes)
 *    id - The pointer to save ID.
 * Returns:
 *    0 on success negative error code on failure (from errno.h).
 */
DISABLE_CFI_ICALL
inline int libcras_node_info_get_id(struct libcras_node_info* node,
                                    uint64_t* id) {
  return node->get_id(node->node_, id);
}

/*
 * Gets device index from the node info pointer.
 * Args:
 *    node - The node info pointer. (Returned from libcras_client_get_nodes)
 *    dev_idx - The pointer to the device index.
 * Returns:
 *    0 on success negative error code on failure (from errno.h).
 */
DISABLE_CFI_ICALL
inline int libcras_node_info_get_dev_idx(struct libcras_node_info* node,
                                         uint32_t* dev_idx) {
  return node->get_dev_idx(node->node_, dev_idx);
}

/*
 * Gets node index from the node info pointer.
 * Args:
 *    node - The node info pointer. (Returned from libcras_client_get_nodes)
 *    node_idx - The pointer to save the node index.
 * Returns:
 *    0 on success negative error code on failure (from errno.h).
 */
DISABLE_CFI_ICALL
inline int libcras_node_info_get_node_idx(struct libcras_node_info* node,
                                          uint32_t* node_idx) {
  return node->get_node_idx(node->node_, node_idx);
}

/*
 * Gets the max supported channels from the node info pointer.
 * Args:
 *    node - The node info pointer. (Returned from libcras_client_get_nodes)
 *    max_supported_channels - The pointer to save the result.
 * Returns:
 *    0 on success negative error code on failure (from errno.h).
 */
DISABLE_CFI_ICALL
inline int libcras_node_info_get_max_supported_channels(
    struct libcras_node_info* node,
    uint32_t* max_supported_channels) {
  return node->get_max_supported_channels(node->node_, max_supported_channels);
}

/*
 * Gets whether the node is plugged from the node info pointer.
 * Args:
 *    node - The node info pointer. (Returned from libcras_client_get_nodes)
 *    plugged - The pointer to save the result.
 * Returns:
 *    0 on success negative error code on failure (from errno.h).
 */
DISABLE_CFI_ICALL
inline int libcras_node_info_is_plugged(struct libcras_node_info* node,
                                        bool* plugged) {
  return node->is_plugged(node->node_, plugged);
}

/*
 * Gets whether the node is active from the node info pointer.
 * Args:
 *    node - The node info pointer. (Returned from libcras_client_get_nodes)
 *    active - The pointer to save the result.
 * Returns:
 *    0 on success negative error code on failure (from errno.h).
 */
DISABLE_CFI_ICALL
inline int libcras_node_info_is_active(struct libcras_node_info* node,
                                       bool* active) {
  return node->is_active(node->node_, active);
}

/*
 * Gets device type from the node info pointer.
 * Args:
 *    node - The node info pointer. (Returned from libcras_client_get_nodes)
 *    type - The pointer to save the device type.
 * Returns:
 *    0 on success negative error code on failure (from errno.h).
 */
DISABLE_CFI_ICALL
inline int libcras_node_info_get_type(struct libcras_node_info* node,
                                      char** type) {
  return node->get_type(node->node_, type);
}

/*
 * Gets device name from the node info pointer.
 * Args:
 *    node - The node info pointer. (Returned from libcras_client_get_nodes)
 *    name - The pointer to save the device name.
 * Returns:
 *    0 on success negative error code on failure (from errno.h).
 */
DISABLE_CFI_ICALL
inline int libcras_node_info_get_node_name(struct libcras_node_info* node,
                                           char** name) {
  return node->get_node_name(node->node_, name);
}

/*
 * Gets node name from the node info pointer.
 * Args:
 *    node - The node info pointer. (Returned from libcras_client_get_nodes)
 *    name - The pointer to save the node name.
 * Returns:
 *    0 on success negative error code on failure (from errno.h).
 */
DISABLE_CFI_ICALL
inline int libcras_node_info_get_dev_name(struct libcras_node_info* node,
                                          char** name) {
  return node->get_dev_name(node->node_, name);
}

#ifdef __cplusplus
}
#endif

#endif  // CRAS_INCLUDE_CRAS_CLIENT_H_
