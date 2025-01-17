// Copyright 2019 The ChromiumOS Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
//! Provides an interface for playing and recording audio through CRAS server.
//!
//! `CrasClient` implements `StreamSource` trait and it can create playback or capture
//! stream - `CrasStream` which can be a
//! - `PlaybackBufferStream` for audio playback or
//! - `CaptureBufferStream` for audio capture.
//!
//! # Example of file audio playback
//!
//! `PlaybackBuffer`s to be filled with audio samples are obtained by calling
//! `next_playback_buffer` from `CrasStream`.
//!
//! Users playing audio fill the provided buffers with audio. When a `PlaybackBuffer` is dropped,
//! the samples written to it are committed to the `CrasStream` it came from.
//!
//!
//! ```
//! // An example of playing raw audio data from a given file
//! use std::env;
//! use std::fs::File;
//! use std::io::{Read, Write};
//! use std::thread::{spawn, JoinHandle};
//! type Result<T> = std::result::Result<T, BoxError>;
//!
//! use libcras::{BoxError, CrasClient, CrasClientType};
//! use audio_streams::{SampleFormat, StreamSource};
//!
//! const BUFFER_SIZE: usize = 256;
//! const FRAME_RATE: u32 = 44100;
//! const NUM_CHANNELS: usize = 2;
//! const FORMAT: SampleFormat = SampleFormat::S16LE;
//!
//! # fn main() -> Result<()> {
//! #    let args: Vec<String> = env::args().collect();
//! #    match args.len() {
//! #        2 => {
//!              let mut cras_client = CrasClient::new()?;
//!              cras_client.set_client_type(CrasClientType::CRAS_CLIENT_TYPE_TEST);
//!              let (_control, mut stream) = cras_client
//!                  .new_playback_stream(NUM_CHANNELS, FORMAT, FRAME_RATE, BUFFER_SIZE)?;
//!
//!              // Plays 1000 * BUFFER_SIZE samples from the given file
//!              let mut file = File::open(&args[1])?;
//!              let mut local_buffer = [0u8; BUFFER_SIZE * NUM_CHANNELS * 2];
//!              for _i in 0..1000 {
//!                  // Reads data to local buffer
//!                  let _read_count = file.read(&mut local_buffer)?;
//!
//!                  // Gets writable buffer from stream and
//!                  let mut buffer = stream.next_playback_buffer()?;
//!                  // Writes data to stream buffer
//!                  let _write_frames = buffer.write(&local_buffer)?;
//!              }
//!              // Stream and client should gracefully be closed out of this scope
//! #        }
//! #        _ => {
//! #            println!("{} /path/to/playback_file.raw", args[0]);
//! #        }
//! #    };
//! #    Ok(())
//! # }
//! ```
//!
//! # Example of file audio capture
//!
//! `CaptureBuffer`s which contain audio samples are obtained by calling
//! `next_capture_buffer` from `CrasStream`.
//!
//! Users get captured audio samples from the provided buffers. When a `CaptureBuffer` is dropped,
//! the number of read samples will be committed to the `CrasStream` it came from.
//! ```
//! use std::env;
//! use std::fs::File;
//! use std::io::{Read, Write};
//! use std::thread::{spawn, JoinHandle};
//! type Result<T> = std::result::Result<T, BoxError>;
//!
//! use libcras::{BoxError, CrasClient, CrasClientType};
//! use audio_streams::{SampleFormat, StreamSource};
//!
//! const BUFFER_SIZE: usize = 256;
//! const FRAME_RATE: u32 = 44100;
//! const NUM_CHANNELS: usize = 2;
//! const FORMAT: SampleFormat = SampleFormat::S16LE;
//!
//! # fn main() -> Result<()> {
//! #    let args: Vec<String> = env::args().collect();
//! #    match args.len() {
//! #        2 => {
//!              let mut cras_client = CrasClient::new()?;
//!              cras_client.set_client_type(CrasClientType::CRAS_CLIENT_TYPE_TEST);
//!              let (_control, mut stream) = cras_client
//!                  .new_capture_stream(NUM_CHANNELS, FORMAT, FRAME_RATE, BUFFER_SIZE, &[])?;
//!
//!              // Capture 1000 * BUFFER_SIZE samples to the given file
//!              let mut file = File::create(&args[1])?;
//!              let mut local_buffer = [0u8; BUFFER_SIZE * NUM_CHANNELS * 2];
//!              for _i in 0..1000 {
//!
//!                  // Gets readable buffer from stream and
//!                  let mut buffer = stream.next_capture_buffer()?;
//!                  // Reads data to local buffer
//!                  let read_count = buffer.read(&mut local_buffer)?;
//!                  // Writes data to file
//!                  let _read_frames = file.write(&local_buffer[..read_count])?;
//!              }
//!              // Stream and client should gracefully be closed out of this scope
//! #        }
//! #        _ => {
//! #            println!("{} /path/to/capture_file.raw", args[0]);
//! #        }
//! #    };
//! #    Ok(())
//! # }
//!  ```

// FIXME: See b/239850356
// This should be removed when that bug is fixed.
#![warn(unaligned_references)]

use std::io;
use std::mem;
use std::os::unix::{
    io::{AsRawFd, RawFd},
    net::UnixStream,
};
use std::{error, fmt};

use async_trait::async_trait;
pub use audio_streams::BoxError;
use audio_streams::{
    capture::{AsyncCaptureBufferStream, CaptureBufferStream, NoopCaptureStream},
    shm_streams::{NullShmStream, SharedMemory, ShmStream, ShmStreamSource},
    AsyncBufferCommit, AsyncPlaybackBufferStream, AudioStreamsExecutor, BufferCommit,
    NoopStreamControl, PlaybackBufferStream, SampleFormat, StreamControl, StreamDirection,
    StreamEffect, StreamSource, StreamSourceGenerator,
};
use cras_sys::gen::*;
pub use cras_sys::gen::{
    CRAS_CLIENT_TYPE as CrasClientType, CRAS_NODE_TYPE as CrasNodeType,
    CRAS_SCREEN_ROTATION as CrasScreenRotation, CRAS_STREAM_EFFECT as CrasStreamEffect,
    CRAS_STREAM_TYPE as CrasStreamType,
};
pub use cras_sys::{
    deserialize_cras_client_type, AudioDebugInfo, CrasIodevInfo, CrasIodevNodeId, CrasIonodeInfo,
    Error as CrasSysError,
};

use libchromeos::deprecated::{PollContext, PollToken};

mod async_;
mod audio_socket;
use crate::audio_socket::AudioSocket;
mod cras_server_socket;
use crate::cras_server_socket::CrasServerSocket;
pub use crate::cras_server_socket::CrasSocketType;
mod cras_shm;
use crate::cras_shm::CrasServerState;
pub mod cras_shm_stream;
use crate::cras_shm_stream::CrasShmStream;
mod cras_stream;
use crate::cras_stream::{CrasCaptureData, CrasPlaybackData, CrasStream, CrasStreamData};
mod cras_client_message;
use crate::cras_client_message::*;

#[derive(Debug)]
pub enum Error {
    CrasClientMessageError(cras_client_message::Error),
    CrasStreamError(cras_stream::Error),
    CrasSysError(cras_sys::Error),
    InvalidCrasSocket,
    IoError(io::Error),
    SysUtilError(libchromeos::sys::Error),
    MessageTypeError,
    UnexpectedExit,
}

impl error::Error for Error {}

impl fmt::Display for Error {
    fn fmt(&self, f: &mut fmt::Formatter) -> fmt::Result {
        match self {
            Error::CrasClientMessageError(ref err) => err.fmt(f),
            Error::CrasStreamError(ref err) => err.fmt(f),
            Error::CrasSysError(ref err) => err.fmt(f),
            Error::InvalidCrasSocket => write!(f, "Invalid cras socket name"),
            Error::IoError(ref err) => err.fmt(f),
            Error::SysUtilError(ref err) => err.fmt(f),
            Error::MessageTypeError => write!(f, "Message type error"),
            Error::UnexpectedExit => write!(f, "Unexpected exit"),
        }
    }
}

type Result<T> = std::result::Result<T, Error>;

impl From<io::Error> for Error {
    fn from(io_err: io::Error) -> Self {
        Error::IoError(io_err)
    }
}

impl From<libchromeos::sys::Error> for Error {
    fn from(sys_util_err: libchromeos::sys::Error) -> Self {
        Error::SysUtilError(sys_util_err)
    }
}

impl From<cras_stream::Error> for Error {
    fn from(err: cras_stream::Error) -> Self {
        Error::CrasStreamError(err)
    }
}

impl From<cras_client_message::Error> for Error {
    fn from(err: cras_client_message::Error) -> Self {
        Error::CrasClientMessageError(err)
    }
}

/// A CRAS server client, which implements StreamSource and ShmStreamSource.
/// It can create audio streams connecting to CRAS server.
pub struct CrasClient<'a> {
    server_socket: CrasServerSocket,
    server_state: CrasServerState<'a>,
    client_id: u32,
    next_stream_id: u32,
    cras_capture: bool,
    client_type: CRAS_CLIENT_TYPE,
    stream_type: CRAS_STREAM_TYPE,
}

impl<'a> CrasClient<'a> {
    /// Blocks creating a `CrasClient` with registered `client_id`
    ///
    /// # Results
    ///
    /// * `CrasClient` - A client to interact with CRAS server
    ///
    /// # Errors
    ///
    /// Returns error if error occurs while handling server message or message
    /// type is incorrect
    pub fn new() -> Result<Self> {
        Self::with_type(CrasSocketType::Legacy)
    }

    /// Tries to create a `CrasClient` with a given `CrasSocketType`.
    ///
    /// # Errors
    ///
    /// Returns error if error occurs while handling server message or message
    /// type is incorrect.
    pub fn with_type(socket_type: CrasSocketType) -> Result<Self> {
        // Create a connection to the server.
        let mut server_socket = CrasServerSocket::with_type(socket_type)?;
        // Gets client ID and server state fd from server
        if let ServerResult::Connected(client_id, server_state_fd) =
            CrasClient::wait_for_message(&mut server_socket)?
        {
            Ok(Self {
                server_socket,
                server_state: CrasServerState::try_new(server_state_fd)?,
                client_id,
                next_stream_id: 0,
                cras_capture: false,
                client_type: CRAS_CLIENT_TYPE::CRAS_CLIENT_TYPE_UNKNOWN,
                stream_type: CRAS_STREAM_TYPE::CRAS_STREAM_TYPE_DEFAULT,
            })
        } else {
            Err(Error::MessageTypeError)
        }
    }

    /// Enables capturing audio through CRAS server.
    pub fn enable_cras_capture(&mut self) {
        self.cras_capture = true;
    }

    /// Set the type of this client to report to CRAS when connecting streams.
    pub fn set_client_type(&mut self, client_type: CRAS_CLIENT_TYPE) {
        self.client_type = client_type;
    }

    /// Set the stream type to report to CRAS when connecting streams.
    ///
    /// All streams created under this client will use this stream type.
    pub fn set_stream_type(&mut self, stream_type: CRAS_STREAM_TYPE) {
        self.stream_type = stream_type;
    }

    /// Sets the system volume to `volume`.
    ///
    /// Send a message to the server to request setting the system volume
    /// to `volume`. No response is returned from the server.
    ///
    /// # Errors
    ///
    /// If writing the message to the server socket failed.
    pub fn set_system_volume(&mut self, volume: u32) -> Result<()> {
        let header = cras_server_message {
            length: mem::size_of::<cras_set_system_volume>() as u32,
            id: CRAS_SERVER_MESSAGE_ID::CRAS_SERVER_SET_SYSTEM_VOLUME,
        };
        let msg = cras_set_system_volume { header, volume };

        self.server_socket.send_server_message_with_fds(&msg, &[])?;
        Ok(())
    }

    /// Sets the system mute status to `mute`.
    ///
    /// Send a message to the server to request setting the system mute
    /// to `mute`. No response is returned from the server.
    ///
    /// # Errors
    ///
    /// If writing the message to the server socket failed.
    pub fn set_system_mute(&mut self, mute: bool) -> Result<()> {
        let header = cras_server_message {
            length: mem::size_of::<cras_set_system_mute>() as u32,
            id: CRAS_SERVER_MESSAGE_ID::CRAS_SERVER_SET_SYSTEM_MUTE,
        };
        let msg = cras_set_system_mute {
            header,
            mute: mute as i32,
        };

        self.server_socket.send_server_message_with_fds(&msg, &[])?;
        Ok(())
    }

    /// Gets the system volume.
    ///
    /// Read the current value for system volume from the server shared memory.
    pub fn get_system_volume(&self) -> u32 {
        self.server_state.get_system_volume()
    }

    /// Gets the system mute.
    ///
    /// Read the current value for system mute from the server shared memory.
    pub fn get_system_mute(&self) -> bool {
        self.server_state.get_system_mute()
    }

    /// Gets a list of output devices
    ///
    /// Read a list of the currently attached output devices from the server shared memory.
    pub fn output_devices(&self) -> impl Iterator<Item = CrasIodevInfo> {
        self.server_state.output_devices()
    }

    /// Gets a list of input devices
    ///
    /// Read a list of the currently attached input devices from the server shared memory.
    pub fn input_devices(&self) -> impl Iterator<Item = CrasIodevInfo> {
        self.server_state.input_devices()
    }

    /// Gets a list of output nodes
    ///
    /// Read a list of the currently attached output nodes from the server shared memory.
    pub fn output_nodes(&self) -> impl Iterator<Item = CrasIonodeInfo> {
        self.server_state.output_nodes()
    }

    /// Gets a list of input nodes
    ///
    /// Read a list of the currently attached input nodes from the server shared memory.
    pub fn input_nodes(&self) -> impl Iterator<Item = CrasIonodeInfo> {
        self.server_state.input_nodes()
    }

    /// Gets the server's audio debug info.
    ///
    /// Sends a message to the server requesting an update of audio debug info,
    /// waits for the response, and then reads the info from the server state.
    ///
    /// # Errors
    ///
    /// * If sending the message to the server failed.
    /// * If an unexpected response message is received.
    pub fn get_audio_debug_info(&mut self) -> Result<AudioDebugInfo> {
        let header = cras_server_message {
            length: mem::size_of::<cras_dump_audio_thread>() as u32,
            id: CRAS_SERVER_MESSAGE_ID::CRAS_SERVER_DUMP_AUDIO_THREAD,
        };
        let msg = cras_dump_audio_thread { header };

        self.server_socket.send_server_message_with_fds(&msg, &[])?;

        match CrasClient::wait_for_message(&mut self.server_socket)? {
            ServerResult::DebugInfoReady => Ok(self
                .server_state
                .get_audio_debug_info()
                .map_err(Error::CrasSysError)?),
            _ => Err(Error::MessageTypeError),
        }
    }

    // Gets next server_stream_id from client and increment stream_id counter.
    fn next_server_stream_id(&mut self) -> u32 {
        let res = self.next_stream_id;
        self.next_stream_id += 1;
        self.server_stream_id(res)
    }

    // Gets server_stream_id from given stream_id
    fn server_stream_id(&self, stream_id: u32) -> u32 {
        (self.client_id << 16) | stream_id
    }

    // Prepares and sends connect stream message to server
    #[allow(clippy::too_many_arguments)]
    fn prepare_and_send_connect_stream(
        &mut self,
        device_index: Option<u32>,
        block_size: u32,
        direction: CRAS_STREAM_DIRECTION,
        rate: u32,
        channel_num: usize,
        format: SampleFormat,
        effects: &[StreamEffect],
        client_shm_size: u64,
        buffer_offsets: [u64; 2],
        fds: &[RawFd],
    ) -> Result<u32> {
        let stream_id = self.next_server_stream_id();

        // Prepares server message
        let audio_format =
            cras_audio_format_packed::new(format.into(), rate, channel_num, direction);
        let msg_header = cras_server_message {
            length: mem::size_of::<cras_connect_message>() as u32,
            id: CRAS_SERVER_MESSAGE_ID::CRAS_SERVER_CONNECT_STREAM,
        };
        let server_cmsg = cras_connect_message {
            header: msg_header,
            proto_version: CRAS_PROTO_VER,
            direction,
            stream_id,
            stream_type: self.stream_type,
            buffer_frames: block_size,
            cb_threshold: block_size,
            flags: 0,
            format: audio_format,
            dev_idx: device_index.unwrap_or(CRAS_SPECIAL_DEVICE::NO_DEVICE as u32),
            effects: effects.iter().collect::<CrasStreamEffect>().into(),
            client_type: self.client_type,
            client_shm_size,
            buffer_offsets,
        };

        // Sends `CRAS_SERVER_CONNECT_STREAM` message
        self.server_socket
            .send_server_message_with_fds(&server_cmsg, fds)?;

        Ok(stream_id)
    }

    // Creates general stream with given parameters
    #[allow(clippy::too_many_arguments)]
    fn create_stream<'b, T: BufferCommit + CrasStreamData<'b>>(
        &mut self,
        device_index: Option<u32>,
        block_size: u32,
        direction: CRAS_STREAM_DIRECTION,
        rate: u32,
        channel_num: usize,
        format: SampleFormat,
        effects: &[StreamEffect],
    ) -> Result<CrasStream<'b, T>> {
        assert!(direction == CRAS_STREAM_DIRECTION::CRAS_STREAM_OUTPUT || self.cras_capture);

        let (sock1, sock2) = UnixStream::pair()?;
        let stream_id = self.prepare_and_send_connect_stream(
            device_index,
            block_size,
            direction,
            rate,
            channel_num,
            format,
            effects,
            0,
            [0, 0],
            &[sock2.as_raw_fd()],
        )?;

        let audio_socket = AudioSocket::new(sock1);
        loop {
            let result = CrasClient::wait_for_message(&mut self.server_socket)?;
            if let ServerResult::StreamConnected(_stream_id, header_fd, samples_fd) = result {
                return CrasStream::try_new(
                    stream_id,
                    self.server_socket.try_clone()?,
                    block_size,
                    direction,
                    rate,
                    channel_num,
                    format.into(),
                    audio_socket,
                    header_fd,
                    samples_fd,
                )
                .map_err(Error::CrasStreamError);
            }
        }
    }

    // Creates general stream with given parameters
    #[allow(clippy::too_many_arguments)]
    fn create_async_stream<'b, T: async_::CrasStreamData<'b> + AsyncBufferCommit>(
        &mut self,
        device_index: Option<u32>,
        block_size: u32,
        direction: CRAS_STREAM_DIRECTION,
        rate: u32,
        channel_num: usize,
        format: SampleFormat,
        effects: &[StreamEffect],
        ex: &dyn AudioStreamsExecutor,
    ) -> Result<async_::CrasStream<'b, T>> {
        assert!(direction == CRAS_STREAM_DIRECTION::CRAS_STREAM_OUTPUT || self.cras_capture);

        let (sock1, sock2) = UnixStream::pair()?;
        let stream_id = self.prepare_and_send_connect_stream(
            device_index,
            block_size,
            direction,
            rate,
            channel_num,
            format,
            effects,
            0,
            [0, 0],
            &[sock2.as_raw_fd()],
        )?;

        let audio_socket = async_::AudioSocket::new(sock1, ex)?;
        loop {
            let result = CrasClient::wait_for_message(&mut self.server_socket)?;
            if let ServerResult::StreamConnected(_stream_id, header_fd, samples_fd) = result {
                return async_::CrasStream::try_new(
                    stream_id,
                    self.server_socket.try_clone()?,
                    block_size,
                    direction,
                    rate,
                    channel_num,
                    format.into(),
                    audio_socket,
                    header_fd,
                    samples_fd,
                )
                .map_err(Error::CrasStreamError);
            }
        }
    }

    // Creates general stream asynchronously with given parameters
    #[allow(clippy::too_many_arguments)]
    async fn async_create_async_stream<'b, T: async_::CrasStreamData<'b> + AsyncBufferCommit>(
        &mut self,
        device_index: Option<u32>,
        block_size: u32,
        direction: CRAS_STREAM_DIRECTION,
        rate: u32,
        channel_num: usize,
        format: SampleFormat,
        effects: &[StreamEffect],
        ex: &dyn AudioStreamsExecutor,
    ) -> Result<async_::CrasStream<'b, T>> {
        assert!(direction == CRAS_STREAM_DIRECTION::CRAS_STREAM_OUTPUT || self.cras_capture);

        let (sock1, sock2) = UnixStream::pair()?;
        let stream_id = self.prepare_and_send_connect_stream(
            device_index,
            block_size,
            direction,
            rate,
            channel_num,
            format,
            effects,
            0,
            [0, 0],
            &[sock2.as_raw_fd()],
        )?;

        let audio_socket = async_::AudioSocket::new(sock1, ex)?;
        loop {
            let result = CrasClient::async_wait_for_message(&mut self.server_socket, ex).await?;
            if let ServerResult::StreamConnected(_stream_id, header_fd, samples_fd) = result {
                return async_::CrasStream::try_new(
                    stream_id,
                    self.server_socket.try_clone()?,
                    block_size,
                    direction,
                    rate,
                    channel_num,
                    format.into(),
                    audio_socket,
                    header_fd,
                    samples_fd,
                )
                .map_err(Error::CrasStreamError);
            }
        }
    }

    /// Creates a new playback stream pinned to the device at `device_index`.
    ///
    /// # Arguments
    ///
    /// * `device_index` - The device to which the stream will be attached.
    /// * `num_channels` - The count of audio channels for the stream.
    /// * `format` - The format to use for stream audio samples.
    /// * `frame_rate` - The sample rate of the stream.
    /// * `buffer_size` - The transfer size granularity in frames.
    #[allow(clippy::type_complexity)]
    pub fn new_pinned_playback_stream(
        &mut self,
        device_index: u32,
        num_channels: usize,
        format: SampleFormat,
        frame_rate: u32,
        buffer_size: usize,
        effects: &[StreamEffect],
    ) -> std::result::Result<(Box<dyn StreamControl>, Box<dyn PlaybackBufferStream>), BoxError>
    {
        Ok((
            Box::new(NoopStreamControl::new()),
            Box::new(self.create_stream::<CrasPlaybackData>(
                Some(device_index),
                buffer_size as u32,
                CRAS_STREAM_DIRECTION::CRAS_STREAM_OUTPUT,
                frame_rate,
                num_channels,
                format,
                effects,
            )?),
        ))
    }

    /// Creates a new capture stream pinned to the device at `device_index`.
    ///
    /// This is useful for, among other things, capturing from a loopback
    /// device.
    ///
    /// # Arguments
    ///
    /// * `device_index` - The device to which the stream will be attached.
    /// * `num_channels` - The count of audio channels for the stream.
    /// * `format` - The format to use for stream audio samples.
    /// * `frame_rate` - The sample rate of the stream.
    /// * `buffer_size` - The transfer size granularity in frames.
    #[allow(clippy::type_complexity)]
    pub fn new_pinned_capture_stream(
        &mut self,
        device_index: u32,
        num_channels: usize,
        format: SampleFormat,
        frame_rate: u32,
        buffer_size: usize,
        effects: &[StreamEffect],
    ) -> std::result::Result<(Box<dyn StreamControl>, Box<dyn CaptureBufferStream>), BoxError> {
        assert!(self.cras_capture);

        Ok((
            Box::new(NoopStreamControl::new()),
            Box::new(self.create_stream::<CrasCaptureData>(
                Some(device_index),
                buffer_size as u32,
                CRAS_STREAM_DIRECTION::CRAS_STREAM_INPUT,
                frame_rate,
                num_channels,
                format,
                effects,
            )?),
        ))
    }

    // Blocks handling the first server message received from `socket`.
    fn wait_for_message(socket: &mut CrasServerSocket) -> Result<ServerResult> {
        #[derive(PollToken)]
        enum Token {
            ServerMsg,
        }
        let poll_ctx: PollContext<Token> =
            PollContext::new().and_then(|pc| pc.add(socket, Token::ServerMsg).and(Ok(pc)))?;

        let events = poll_ctx.wait()?;
        // Check the first readable message
        let tokens: Vec<Token> = events.iter_readable().map(|e| e.token()).collect();
        tokens
            .get(0)
            .ok_or(Error::UnexpectedExit)
            .and_then(|ref token| {
                match token {
                    Token::ServerMsg => ServerResult::handle_server_message(socket),
                }
                .map_err(Into::into)
            })
    }

    async fn async_wait_for_message(
        socket: &mut CrasServerSocket,
        ex: &dyn AudioStreamsExecutor,
    ) -> Result<ServerResult> {
        ex.wait_fd_readable(socket.try_clone()?.as_raw_fd()).await?;
        Self::wait_for_message(socket)
    }

    /// Returns any open file descriptors needed by CrasClient.
    /// This function is shared between StreamSource and ShmStreamSource.
    fn keep_fds(&self) -> Vec<RawFd> {
        vec![self.server_socket.as_raw_fd()]
    }
}

#[async_trait(?Send)]
impl<'a> StreamSource for CrasClient<'a> {
    #[allow(clippy::type_complexity)]
    fn new_playback_stream(
        &mut self,
        num_channels: usize,
        format: SampleFormat,
        frame_rate: u32,
        buffer_size: usize,
    ) -> std::result::Result<(Box<dyn StreamControl>, Box<dyn PlaybackBufferStream>), BoxError>
    {
        Ok((
            Box::new(NoopStreamControl::new()),
            Box::new(self.create_stream::<CrasPlaybackData>(
                None,
                buffer_size as u32,
                CRAS_STREAM_DIRECTION::CRAS_STREAM_OUTPUT,
                frame_rate,
                num_channels,
                format,
                &[],
            )?),
        ))
    }

    #[allow(clippy::type_complexity)]
    fn new_async_playback_stream(
        &mut self,
        num_channels: usize,
        format: SampleFormat,
        frame_rate: u32,
        buffer_size: usize,
        ex: &dyn AudioStreamsExecutor,
    ) -> std::result::Result<(Box<dyn StreamControl>, Box<dyn AsyncPlaybackBufferStream>), BoxError>
    {
        Ok((
            Box::new(NoopStreamControl::new()),
            Box::new(self.create_async_stream::<async_::CrasPlaybackData>(
                None,
                buffer_size as u32,
                CRAS_STREAM_DIRECTION::CRAS_STREAM_OUTPUT,
                frame_rate,
                num_channels,
                format,
                &[],
                ex,
            )?),
        ))
    }

    #[allow(clippy::type_complexity)]
    async fn async_new_async_playback_stream(
        &mut self,
        num_channels: usize,
        format: SampleFormat,
        frame_rate: u32,
        buffer_size: usize,
        ex: &dyn AudioStreamsExecutor,
    ) -> std::result::Result<(Box<dyn StreamControl>, Box<dyn AsyncPlaybackBufferStream>), BoxError>
    {
        Ok((
            Box::new(NoopStreamControl::new()),
            Box::new(
                self.async_create_async_stream::<async_::CrasPlaybackData>(
                    None,
                    buffer_size as u32,
                    CRAS_STREAM_DIRECTION::CRAS_STREAM_OUTPUT,
                    frame_rate,
                    num_channels,
                    format,
                    &[],
                    ex,
                )
                .await?,
            ),
        ))
    }

    #[allow(clippy::type_complexity)]
    fn new_capture_stream(
        &mut self,
        num_channels: usize,
        format: SampleFormat,
        frame_rate: u32,
        buffer_size: usize,
        effects: &[StreamEffect],
    ) -> std::result::Result<(Box<dyn StreamControl>, Box<dyn CaptureBufferStream>), BoxError> {
        if self.cras_capture {
            Ok((
                Box::new(NoopStreamControl::new()),
                Box::new(self.create_stream::<CrasCaptureData>(
                    None,
                    buffer_size as u32,
                    CRAS_STREAM_DIRECTION::CRAS_STREAM_INPUT,
                    frame_rate,
                    num_channels,
                    format,
                    effects,
                )?),
            ))
        } else {
            Ok((
                Box::new(NoopStreamControl::new()),
                Box::new(NoopCaptureStream::new(
                    num_channels,
                    format,
                    frame_rate,
                    buffer_size,
                )),
            ))
        }
    }

    #[allow(clippy::type_complexity)]
    fn new_async_capture_stream(
        &mut self,
        num_channels: usize,
        format: SampleFormat,
        frame_rate: u32,
        buffer_size: usize,
        effects: &[StreamEffect],
        ex: &dyn AudioStreamsExecutor,
    ) -> std::result::Result<(Box<dyn StreamControl>, Box<dyn AsyncCaptureBufferStream>), BoxError>
    {
        if self.cras_capture {
            Ok((
                Box::new(NoopStreamControl::new()),
                Box::new(self.create_async_stream::<async_::CrasCaptureData>(
                    None,
                    buffer_size as u32,
                    CRAS_STREAM_DIRECTION::CRAS_STREAM_INPUT,
                    frame_rate,
                    num_channels,
                    format,
                    effects,
                    ex,
                )?),
            ))
        } else {
            Ok((
                Box::new(NoopStreamControl::new()),
                Box::new(NoopCaptureStream::new(
                    num_channels,
                    format,
                    frame_rate,
                    buffer_size,
                )),
            ))
        }
    }

    #[allow(clippy::type_complexity)]
    async fn async_new_async_capture_stream(
        &mut self,
        num_channels: usize,
        format: SampleFormat,
        frame_rate: u32,
        buffer_size: usize,
        effects: &[StreamEffect],
        ex: &dyn AudioStreamsExecutor,
    ) -> std::result::Result<(Box<dyn StreamControl>, Box<dyn AsyncCaptureBufferStream>), BoxError>
    {
        if self.cras_capture {
            Ok((
                Box::new(NoopStreamControl::new()),
                Box::new(
                    self.async_create_async_stream::<async_::CrasCaptureData>(
                        None,
                        buffer_size as u32,
                        CRAS_STREAM_DIRECTION::CRAS_STREAM_INPUT,
                        frame_rate,
                        num_channels,
                        format,
                        effects,
                        ex,
                    )
                    .await?,
                ),
            ))
        } else {
            Ok((
                Box::new(NoopStreamControl::new()),
                Box::new(NoopCaptureStream::new(
                    num_channels,
                    format,
                    frame_rate,
                    buffer_size,
                )),
            ))
        }
    }

    fn keep_rds(&self) -> Option<Vec<RawFd>> {
        Some(CrasClient::keep_fds(self))
    }
}

impl<'a, E: std::error::Error> ShmStreamSource<E> for CrasClient<'a> {
    fn new_stream(
        &mut self,
        direction: StreamDirection,
        num_channels: usize,
        format: SampleFormat,
        frame_rate: u32,
        buffer_size: usize,
        effects: &[StreamEffect],
        client_shm: &dyn SharedMemory<Error = E>,
        buffer_offsets: [u64; 2],
    ) -> std::result::Result<Box<dyn ShmStream>, BoxError> {
        if direction == StreamDirection::Capture && !self.cras_capture {
            return Ok(Box::new(NullShmStream::new(
                buffer_size,
                num_channels,
                format,
                frame_rate,
            )));
        }

        let (sock1, sock2) = UnixStream::pair()?;
        let stream_id = self.prepare_and_send_connect_stream(
            None,
            buffer_size as u32,
            direction.into(),
            frame_rate,
            num_channels,
            format,
            effects,
            client_shm.size(),
            buffer_offsets,
            &[sock2.as_raw_fd(), client_shm.as_raw_fd()],
        )?;

        loop {
            let result = CrasClient::wait_for_message(&mut self.server_socket)?;
            if let ServerResult::StreamConnected(_stream_id, header_fd, _samples_fd) = result {
                let audio_socket = AudioSocket::new(sock1);
                let stream = CrasShmStream::try_new(
                    stream_id,
                    self.server_socket.try_clone()?,
                    audio_socket,
                    direction,
                    num_channels,
                    frame_rate,
                    format,
                    header_fd,
                    client_shm.size() as usize,
                )?;
                return Ok(Box::new(stream));
            }
        }
    }

    fn keep_fds(&self) -> Vec<RawFd> {
        CrasClient::keep_fds(self)
    }
}

/// A struct that implements [`StreamSourceGenerator`].
/// It can create a CRAS Client, which implements [`StreamSource`] to connect to CRAS server.
pub struct CrasStreamSourceGenerator {
    capture: bool,
    client_type: CrasClientType,
    socket_type: CrasSocketType,
    stream_type: CrasStreamType,
}

impl CrasStreamSourceGenerator {
    /// Blocks creating a new `CrasStreamSourceGenerator`. Streams from this stream source always
    /// use stream type `CrasStreamType::CRAS_STREAM_TYPE_DEFAULT` when connecting to CRAS.
    /// If you want to use different stream type, use `with_stream_type`.
    ///
    /// # Arguments
    ///
    /// * `capture` - Indicator to enable/disable audio capture through CRAS server.
    /// * `client_type` - Type of the CRAS Client that we want to create.
    /// * `socket_type` - Type of the socket for the CRAS Client to connect.
    pub fn new(capture: bool, client_type: CrasClientType, socket_type: CrasSocketType) -> Self {
        Self::with_stream_type(
            capture,
            client_type,
            socket_type,
            CrasStreamType::CRAS_STREAM_TYPE_DEFAULT,
        )
    }

    /// Blocks creating a new `CrasStreamSourceGenerator` with stream_type.
    ///
    /// # Arguments
    ///
    /// * `capture` - Indicator to enable/disable audio capture through CRAS server.
    /// * `client_type` - Type of the CRAS Client that we want to create.
    /// * `socket_type` - Type of the socket for the CRAS Client to connect.
    /// * `stream_type` - Type of stream to send to CRAS when connecting streams.
    pub fn with_stream_type(
        capture: bool,
        client_type: CrasClientType,
        socket_type: CrasSocketType,
        stream_type: CrasStreamType,
    ) -> Self {
        CrasStreamSourceGenerator {
            capture,
            client_type,
            socket_type,
            stream_type,
        }
    }
}

impl StreamSourceGenerator for CrasStreamSourceGenerator {
    fn generate(&self) -> std::result::Result<Box<dyn StreamSource>, BoxError> {
        let mut client = CrasClient::with_type(self.socket_type)?;
        if self.capture {
            client.enable_cras_capture();
        }
        client.set_client_type(self.client_type);
        client.set_stream_type(self.stream_type);
        Ok(Box::new(client))
    }
}
