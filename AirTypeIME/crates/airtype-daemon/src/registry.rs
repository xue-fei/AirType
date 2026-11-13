//! 设备注册表：记录每个 AirMic 连接的状态与最近识别统计，给设置页面用。
//!
//! 用 `std::sync::Mutex` 而不是 tokio 的锁：临界区只有几次 HashMap 操作，
//! 持锁时间是微秒级，不会卡住 async 执行器。

use std::collections::HashMap;
use std::sync::Mutex;
use std::time::Instant;

use serde::Serialize;

/// 最多保留多少个历史设备条目，避免长时间运行后无限增长。
const MAX_ENTRIES: usize = 32;

/// 设置页面 `/api/state` 里的一个设备条目。
#[derive(Debug, Clone, Serialize)]
pub struct DeviceStatus {
    /// START 帧元数据里的 device 字段（MAC 生成的 `airmic-XXXXXX`）。
    pub device: String,
    /// 对端地址，例如 `192.168.2.177:54321`。
    pub peer: String,
    pub connected: bool,
    /// 距上次收到该连接数据的毫秒数。
    pub idle_ms: u64,
    pub utterances: u64,
    pub last_text: String,
    pub last_first_partial_ms: u64,
    pub last_total_ms: u64,
}

#[derive(Debug)]
struct Entry {
    device: String,
    connected: bool,
    last_seen: Instant,
    utterances: u64,
    last_text: String,
    last_first_partial_ms: u64,
    last_total_ms: u64,
}

impl Entry {
    fn new(device: &str) -> Self {
        Self {
            device: device.to_string(),
            connected: true,
            last_seen: Instant::now(),
            utterances: 0,
            last_text: String::new(),
            last_first_partial_ms: 0,
            last_total_ms: 0,
        }
    }
}

#[derive(Debug, Default)]
pub struct Registry {
    entries: Mutex<HashMap<String, Entry>>,
}

impl Registry {
    pub fn new() -> Self {
        Self::default()
    }

    /// 新连接（此时还不知道 device 名，用 peer 占位）。
    pub fn on_connected(&self, peer: &str) {
        let mut map = self.entries.lock().unwrap_or_else(|e| e.into_inner());
        let entry = map.entry(peer.to_string()).or_insert_with(|| Entry::new(peer));
        entry.connected = true;
        entry.last_seen = Instant::now();
    }

    /// START 帧带来 device 名，补进已有条目。
    pub fn on_device(&self, peer: &str, device: &str) {
        if device.is_empty() {
            return;
        }
        let mut map = self.entries.lock().unwrap_or_else(|e| e.into_inner());
        let entry = map.entry(peer.to_string()).or_insert_with(|| Entry::new(device));
        entry.device = device.to_string();
    }

    /// 一次识别完成，记下文本与延迟。
    pub fn on_utterance(
        &self,
        peer: &str,
        text: &str,
        first_partial_ms: u64,
        total_ms: u64,
    ) {
        let mut map = self.entries.lock().unwrap_or_else(|e| e.into_inner());
        let entry = map.entry(peer.to_string()).or_insert_with(|| Entry::new(peer));
        entry.utterances += 1;
        entry.last_text = text.to_string();
        entry.last_first_partial_ms = first_partial_ms;
        entry.last_total_ms = total_ms;
        entry.last_seen = Instant::now();
    }

    pub fn on_disconnected(&self, peer: &str) {
        let mut map = self.entries.lock().unwrap_or_else(|e| e.into_inner());
        if let Some(entry) = map.get_mut(peer) {
            entry.connected = false;
            entry.last_seen = Instant::now();
        }
    }

    /// 当前快照：在线的排前面，同组内按最近活动时间倒序。
    pub fn snapshot(&self) -> Vec<DeviceStatus> {
        let mut map = self.entries.lock().unwrap_or_else(|e| e.into_inner());
        prune(&mut map);

        let mut out: Vec<DeviceStatus> = map
            .iter()
            .map(|(peer, e)| DeviceStatus {
                device: e.device.clone(),
                peer: peer.clone(),
                connected: e.connected,
                idle_ms: e.last_seen.elapsed().as_millis() as u64,
                utterances: e.utterances,
                last_text: e.last_text.clone(),
                last_first_partial_ms: e.last_first_partial_ms,
                last_total_ms: e.last_total_ms,
            })
            .collect();

        out.sort_by(|a, b| {
            b.connected
                .cmp(&a.connected)
                .then_with(|| b.idle_ms.cmp(&a.idle_ms))
        });
        out
    }
}

/// 超上限时优先丢掉「已断开且最久没活动」的条目。
fn prune(map: &mut HashMap<String, Entry>) {
    if map.len() <= MAX_ENTRIES {
        return;
    }
    let mut victims: Vec<(String, Instant)> = map
        .iter()
        .filter(|(_, e)| !e.connected)
        .map(|(k, e)| (k.clone(), e.last_seen))
        .collect();
    victims.sort_by_key(|(_, t)| *t);
    for (k, _) in victims.iter().take(map.len() - MAX_ENTRIES) {
        map.remove(k);
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn connected_device_shows_up() {
        let r = Registry::new();
        r.on_connected("10.0.0.2:1111");
        r.on_device("10.0.0.2:1111", "airmic-abcdef");
        let snap = r.snapshot();
        assert_eq!(snap.len(), 1);
        assert_eq!(snap[0].device, "airmic-abcdef");
        assert!(snap[0].connected);
        assert_eq!(snap[0].utterances, 0);
    }

    #[test]
    fn utterances_are_counted() {
        let r = Registry::new();
        r.on_connected("10.0.0.2:1111");
        r.on_utterance("10.0.0.2:1111", "你好", 120, 300);
        r.on_utterance("10.0.0.2:1111", "世界", 90, 250);
        let snap = r.snapshot();
        assert_eq!(snap[0].utterances, 2);
        assert_eq!(snap[0].last_text, "世界");
        assert_eq!(snap[0].last_first_partial_ms, 90);
        assert_eq!(snap[0].last_total_ms, 250);
    }

    #[test]
    fn disconnected_stays_in_snapshot() {
        let r = Registry::new();
        r.on_connected("10.0.0.2:1111");
        r.on_utterance("10.0.0.2:1111", "你好", 100, 200);
        r.on_disconnected("10.0.0.2:1111");
        let snap = r.snapshot();
        assert_eq!(snap.len(), 1, "断开的设备仍保留，方便看最后状态");
        assert!(!snap[0].connected);
        assert_eq!(snap[0].last_text, "你好");
    }

    #[test]
    fn online_sorts_before_offline() {
        let r = Registry::new();
        r.on_connected("10.0.0.2:1111");
        r.on_connected("10.0.0.2:2222");
        r.on_disconnected("10.0.0.2:1111");
        let snap = r.snapshot();
        assert_eq!(snap[0].peer, "10.0.0.2:2222", "在线的排前面");
        assert_eq!(snap[1].peer, "10.0.0.2:1111");
    }

    #[test]
    fn entries_are_capped() {
        let r = Registry::new();
        for i in 0..(MAX_ENTRIES + 10) {
            let peer = format!("10.0.0.2:{i}");
            r.on_connected(&peer);
            r.on_disconnected(&peer);
        }
        assert!(r.snapshot().len() <= MAX_ENTRIES);
    }

    #[test]
    fn unknown_device_name_is_ignored() {
        let r = Registry::new();
        r.on_connected("10.0.0.2:1111");
        r.on_device("10.0.0.2:1111", "");
        assert_eq!(r.snapshot()[0].device, "10.0.0.2:1111");
    }
}
