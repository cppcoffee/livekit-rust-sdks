use livekit::{options::{TrackPublishOptions, VideoCodec}, Room, RoomOptions, RoomEvent};
use livekit_api::access_token;
use std::env;
use livekit::track::{LocalTrack, LocalVideoTrack, TrackSource};
use livekit::webrtc::desktop_capturer::{CaptureError, DesktopCaptureSourceType, DesktopCapturer, DesktopCapturerOptions, DesktopFrame};
use livekit::webrtc::native::yuv_helper;
use livekit::webrtc::prelude::{I420Buffer, RtcVideoSource, VideoBuffer, VideoFrame, VideoResolution, VideoRotation};
use livekit::webrtc::video_source::native::NativeVideoSource;
use std::sync::{Arc, Mutex, Condvar};
use std::sync::mpsc::{self, RecvTimeoutError, Sender};
use std::time::Duration;
use serde::{Deserialize, Serialize};

#[derive(Serialize, Deserialize, Debug)]
#[serde(tag = "type")]
enum ControlEvent {
    MouseMove { x: i32, y: i32 },
    MouseClick { button: String, down: bool },
    KeyPress { key: String, down: bool },
}

type ResolutionSignal = Arc<(Mutex<Option<VideoResolution>>, Condvar)>;
type VideoSourceSlot = Arc<Mutex<Option<NativeVideoSource>>>;

enum CaptureCommand {
    Terminate,
}

fn wait_for_resolution(signal: &ResolutionSignal) -> VideoResolution {
    let (lock, cvar) = &**signal;
    let mut guard = lock.lock().unwrap();
    while guard.is_none() {
        guard = cvar.wait(guard).unwrap();
    }
    guard.clone().unwrap()
}

fn spawn_capture_thread(
    source_type: DesktopCaptureSourceType,
    resolution_signal: ResolutionSignal,
    video_source_slot: VideoSourceSlot,
) -> (Sender<CaptureCommand>, std::thread::JoinHandle<()>) {
    let (command_tx, command_rx) = mpsc::channel();
    let handle = std::thread::spawn(move || {
        run_capture_loop(
            source_type,
            resolution_signal,
            video_source_slot,
            command_rx,
        )
    });
    (command_tx, handle)
}

fn run_capture_loop(
    source_type: DesktopCaptureSourceType,
    resolution_signal: ResolutionSignal,
    video_source_slot: VideoSourceSlot,
    command_rx: mpsc::Receiver<CaptureCommand>,
) {
    let callback = {
        let mut frame_buffer = VideoFrame {
            rotation: VideoRotation::VideoRotation0,
            buffer: I420Buffer::new(1, 1),
            timestamp_us: 0,
        };
        move |result: Result<DesktopFrame, CaptureError>| {
            let frame = match result {
                Ok(frame) => frame,
                _ => return,
            };

            let width = frame.width();
            let height = frame.height();
            let stride = frame.stride();
            let data = frame.data();

            {
                let (lock, cvar) = &*resolution_signal;
                let mut guard = lock.lock().unwrap();
                if guard.is_none() {
                    *guard = Some(VideoResolution { width: width as u32, height: height as u32 });
                    cvar.notify_all();
                }
            }

            let buffer_width = frame_buffer.buffer.width() as i32;
            let buffer_height = frame_buffer.buffer.height() as i32;
            if buffer_width != width || buffer_height != height {
                frame_buffer.buffer = I420Buffer::new(width as u32, height as u32);
            }

            let (stride_y, stride_u, stride_v) = frame_buffer.buffer.strides();
            let (y_plane, u_plane, v_plane) = frame_buffer.buffer.data_mut();
            yuv_helper::argb_to_i420(
                data, stride, y_plane, stride_y, u_plane, stride_u, v_plane, stride_v, width, height,
            );

            let slot = video_source_slot.lock().unwrap();
            if let Some(source) = slot.as_ref() {
                source.capture_frame(&frame_buffer);
            }
        }
    };

    let mut options = DesktopCapturerOptions::new(source_type);
    options.set_include_cursor(true);

    let mut capturer = DesktopCapturer::new(options).expect("Failed to create desktop capturer");
    let sources = capturer.get_source_list();
    let selected_source = sources.first().cloned();

    capturer.start_capture(selected_source, callback);

    loop {
        match command_rx.recv_timeout(Duration::from_millis(33)) {
            Ok(CaptureCommand::Terminate) => break,
            Err(RecvTimeoutError::Timeout) => {
                capturer.capture_frame();
            }
            Err(RecvTimeoutError::Disconnected) => break,
        }
    }
}

async fn handle_input_events(mut rx: tokio::sync::mpsc::UnboundedReceiver<RoomEvent>) {
    let mut enigo = enigo::Enigo::new(&enigo::Settings::default()).expect("Failed to init Enigo");

    while let Some(event) = rx.recv().await {
        if let RoomEvent::DataReceived { payload, .. } = event {
            if let Ok(event) = serde_json::from_slice::<ControlEvent>(&payload) {
                use enigo::{Mouse, Keyboard};
                match event {
                    ControlEvent::MouseMove { x, y } => {
                        let _ = enigo.move_mouse(x, y, enigo::Coordinate::Abs);
                    }
                    ControlEvent::MouseClick { button, down } => {
                        let btn = match button.as_str() {
                            "left" => enigo::Button::Left,
                            "right" => enigo::Button::Right,
                            "middle" => enigo::Button::Middle,
                            _ => continue,
                        };
                        let _ = enigo.button(btn, if down { enigo::Direction::Press } else { enigo::Direction::Release });
                    }
                    ControlEvent::KeyPress { key, down } => {
                        let parsed_key = if key.len() == 1 {
                            enigo::Key::Unicode(key.chars().next().unwrap())
                        } else {
                            continue;
                        };
                        let _ = enigo.key(parsed_key, if down { enigo::Direction::Press } else { enigo::Direction::Release });
                    }
                }
            }
        }
    }
}

#[tokio::main]
async fn main() -> Result<(), Box<dyn std::error::Error>> {
    env_logger::Builder::from_env(env_logger::Env::default().default_filter_or("webrtc=debug,info")).init();
    log::info!("Probing hardware encoders (NVENC/VAAPI) via WebRTC debug logs...");

    let url = env::var("LIVEKIT_URL").expect("LIVEKIT_URL is not set");
    let api_key = env::var("LIVEKIT_API_KEY").expect("LIVEKIT_API_KEY is not set");
    let api_secret = env::var("LIVEKIT_API_SECRET").expect("LIVEKIT_API_SECRET is not set");

    let token = access_token::AccessToken::with_api_key(&api_key, &api_secret)
        .with_identity("remote-host")
        .with_name("Remote Host")
        .with_grants(access_token::VideoGrants {
            room_join: true,
            room: "remote_control_room".to_string(),
            ..Default::default()
        })
        .to_jwt()?;

    log::info!("Connecting to LiveKit room...");
    let (room, rx) = Room::connect(&url, &token, RoomOptions::default()).await?;
    log::info!("Connected to room: {} - {}", room.name(), room.sid().await);

    let resolution_signal: ResolutionSignal = Arc::new((Mutex::new(None), Condvar::new()));
    let video_source_slot: VideoSourceSlot = Arc::new(Mutex::new(None));
    let (_capture_cmd_tx, _capture_handle) = spawn_capture_thread(
        DesktopCaptureSourceType::Screen,
        resolution_signal.clone(),
        video_source_slot.clone(),
    );

    let resolution = wait_for_resolution(&resolution_signal);
    log::info!("Detected capture resolution: {}x{}", resolution.width, resolution.height);

    let buffer_source = NativeVideoSource::new(resolution.clone(), true);
    {
        let mut slot = video_source_slot.lock().unwrap();
        *slot = Some(buffer_source.clone());
    }

    let track = LocalVideoTrack::create_video_track(
        "remote_desktop",
        RtcVideoSource::Native(buffer_source.clone()),
    );

    log::info!("Publishing track...");
    room.local_participant().publish_track(
        LocalTrack::Video(track),
        TrackPublishOptions {
            source: TrackSource::Screenshare,
            video_codec: VideoCodec::H264,
            ..Default::default()
        },
    ).await?;
    log::info!("Track published successfully");

    tokio::spawn(handle_input_events(rx));
    std::future::pending::<()>().await;

    Ok(())
}
