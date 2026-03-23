use eframe::egui;

use livekit::{Room, RoomOptions, RoomEvent};
use livekit_api::access_token;
use std::env;
use std::sync::Arc;
use std::sync::Mutex;
use livekit::participant::LocalParticipant;
use livekit::track::RemoteTrack;
use livekit::webrtc::video_stream::native::NativeVideoStream;
use crossbeam_channel::{unbounded, Sender};
use livekit::webrtc::prelude::VideoFrame;
use livekit::webrtc::prelude::I420Buffer;
use livekit::webrtc::prelude::VideoBuffer;
use tokio_stream::StreamExt;
use livekit::webrtc::native::yuv_helper;

use serde::{Deserialize, Serialize};

#[derive(Serialize, Deserialize, Debug)]
#[serde(tag = "type")]
enum ControlEvent {
    MouseMove { x: i32, y: i32 },
    MouseClick { button: String, down: bool },
    KeyPress { key: String, down: bool },
}

#[derive(Default)]
struct AppState {
    local_participant: Option<LocalParticipant>,
    texture: Option<egui::TextureHandle>,
    last_width: u32,
    last_height: u32,
}

fn main() -> Result<(), eframe::Error> {
    env_logger::Builder::from_env(env_logger::Env::default().default_filter_or("info")).init();

    let options = eframe::NativeOptions::default();
    let (tx, rx_frames) = unbounded();
    let app_state = Arc::new(Mutex::new(AppState { local_participant: None, texture: None, last_width: 0, last_height: 0 }));
    let app_state_clone = app_state.clone();

    let rt = tokio::runtime::Runtime::new().unwrap();
    let rt_handle = rt.handle().clone();

    std::thread::spawn(move || {
        rt.block_on(async {
            let _ = setup_room(app_state_clone, tx).await;
        });
    });

    eframe::run_native(
        "Remote Control Client",
        options,
        Box::new(move |_cc| Ok(Box::new(RemoteControlApp { state: app_state, rx_frames, rt_handle }))),
    )
}

struct RemoteControlApp {
    state: Arc<Mutex<AppState>>,
    rx_frames: crossbeam_channel::Receiver<VideoFrame<I420Buffer>>,
    rt_handle: tokio::runtime::Handle,
}

impl eframe::App for RemoteControlApp {
    fn update(&mut self, ctx: &egui::Context, _frame: &mut eframe::Frame) {
        let mut frames = Vec::new();
        while let Ok(frame) = self.rx_frames.try_recv() {
            frames.push(frame);
        }

        let mut state = self.state.lock().unwrap();

        if let Some(frame) = frames.pop() {
            let width = frame.buffer.width() as u32;
            let height = frame.buffer.height() as u32;
            state.last_width = width;
            state.last_height = height;
            let mut rgba_data = vec![0u8; (width * height * 4) as usize];

            let (stride_y, stride_u, stride_v) = frame.buffer.strides();
            let (data_y, data_u, data_v) = frame.buffer.data();

            unsafe {
                yuv_helper::i420_to_rgba(
                    data_y, stride_y,
                    data_u, stride_u,
                    data_v, stride_v,
                    &mut rgba_data, ((width * 4) as u32).try_into().unwrap(),
                    width as i32, height as i32,
                );
            }

            let color_image = egui::ColorImage::from_rgba_unmultiplied([width as usize, height as usize], &rgba_data);
            state.texture = Some(ctx.load_texture("video_frame", color_image, egui::TextureOptions::default()));
        }

        egui::CentralPanel::default().show(ctx, |ui| {
            if let Some(texture) = &state.texture {
                let response = ui.add(egui::Image::new(texture));

                let rect = response.rect;
                let scale_x = state.last_width as f32 / rect.width();
                let scale_y = state.last_height as f32 / rect.height();

                let handle_clone = self.rt_handle.clone();
                let send_event = |event: ControlEvent| {
                    if let Some(p) = &state.local_participant {
                        if let Ok(json) = serde_json::to_vec(&event) {
                            let p_clone = p.clone();
                            handle_clone.spawn(async move {
                                let packet = livekit::DataPacket {
                                    payload: json,
                                    topic: Some("control".to_string()),
                                    destination_identities: vec![],
                                    reliable: true,
                                };
                                let _ = p_clone.publish_data(packet).await;
                            });
                        }
                    }
                };

                if let Some(hover_pos) = response.hover_pos() {
                    let local_x = (hover_pos.x - rect.left()) * scale_x;
                    let local_y = (hover_pos.y - rect.top()) * scale_y;

                    send_event(ControlEvent::MouseMove { x: local_x as i32, y: local_y as i32 });
                }

                if response.clicked() {
                    send_event(ControlEvent::MouseClick { button: "left".to_string(), down: true });
                    send_event(ControlEvent::MouseClick { button: "left".to_string(), down: false });
                }
                if response.secondary_clicked() {
                    send_event(ControlEvent::MouseClick { button: "right".to_string(), down: true });
                    send_event(ControlEvent::MouseClick { button: "right".to_string(), down: false });
                }

                ctx.input(|i| {
                    for event in i.events.iter() {
                        if let egui::Event::Key { key, pressed, .. } = event {
                            let key_str = format!("{:?}", key); // Simplistic key translation
                            send_event(ControlEvent::KeyPress { key: key_str, down: *pressed });
                        }
                    }
                });

            } else {
                ui.label("Waiting for video stream...");
            }
        });

        ctx.request_repaint(); // Continuously repaint for video
    }
}

async fn setup_room(state: Arc<Mutex<AppState>>, tx: crossbeam_channel::Sender<VideoFrame<I420Buffer>>) -> Result<(), Box<dyn std::error::Error>> {
    let url = env::var("LIVEKIT_URL").expect("LIVEKIT_URL is not set");
    let api_key = env::var("LIVEKIT_API_KEY").expect("LIVEKIT_API_KEY is not set");
    let api_secret = env::var("LIVEKIT_API_SECRET").expect("LIVEKIT_API_SECRET is not set");

    let token = access_token::AccessToken::with_api_key(&api_key, &api_secret)
        .with_identity("remote-client")
        .with_name("Remote Client")
        .with_grants(access_token::VideoGrants {
            room_join: true,
            room: "remote_control_room".to_string(),
            ..Default::default()
        })
        .to_jwt()?;

    log::info!("Connecting to LiveKit room...");
    let (room, rx) = Room::connect(&url, &token, RoomOptions::default()).await?;
    log::info!("Connected to room: {} - {}", room.name(), room.sid().await);

    {
        let mut app_state = state.lock().unwrap();
        app_state.local_participant = Some(room.local_participant());
    }
    tokio::spawn(handle_room_events(rx, tx));

    Ok(())
}

async fn handle_room_events(mut rx: tokio::sync::mpsc::UnboundedReceiver<RoomEvent>, tx: Sender<VideoFrame<I420Buffer>>) {
    while let Some(event) = rx.recv().await {
        if let RoomEvent::TrackSubscribed { track, .. } = event {
            if let RemoteTrack::Video(video_track) = track {
                let mut video_stream = NativeVideoStream::new(video_track.rtc_track());
                let tx = tx.clone();
                tokio::spawn(async move {
                    while let Some(frame) = video_stream.next().await {
                        let i420_frame = VideoFrame {
                            rotation: frame.rotation,
                            timestamp_us: frame.timestamp_us,
                            buffer: frame.buffer.to_i420(),
                        };
                        let _ = tx.send(i420_frame);
                    }
                });
            }
        }
    }
}
