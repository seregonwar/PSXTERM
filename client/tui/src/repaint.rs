//! Bound busy redraws and avoid drawing an unchanged workspace during idle polls.
use std::time::{Duration, Instant};

const FRAME_INTERVAL: Duration = Duration::from_micros(16_667);
const IDLE_POLL: Duration = Duration::from_millis(33);

pub struct Repaint {
    pending: bool,
    next_frame: Instant,
}
impl Repaint {
    pub fn new(now: Instant) -> Self {
        Self {
            pending: true,
            next_frame: now,
        }
    }
    pub fn request(&mut self) {
        self.pending = true;
    }
    pub fn urgent(&mut self, now: Instant) {
        self.pending = true;
        self.next_frame = now;
    }
    pub fn ready(&self, now: Instant) -> bool {
        self.pending && now >= self.next_frame
    }
    pub fn rendered(&mut self, now: Instant) {
        self.pending = false;
        self.next_frame = now + FRAME_INTERVAL;
    }
    pub fn wait(&self, now: Instant) -> Duration {
        if self.pending {
            self.next_frame
                .saturating_duration_since(now)
                .min(IDLE_POLL)
        } else {
            IDLE_POLL
        }
    }
    pub fn wait_for_work(&self, now: Instant, pending: bool) -> Duration {
        let wait = self.wait(now);
        if pending {
            wait.min(Duration::from_millis(1))
        } else {
            wait
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn queued_background_work_is_polled_promptly_without_requesting_frames() {
        let now = Instant::now();
        let mut repaint = Repaint::new(now);
        repaint.rendered(now);
        assert_eq!(repaint.wait_for_work(now, true), Duration::from_millis(1));
        assert!(!repaint.ready(now + IDLE_POLL));
        assert_eq!(repaint.wait_for_work(now + IDLE_POLL, false), IDLE_POLL);
    }
    #[test]
    fn idle_polls_do_not_request_frames_and_requests_coalesce() {
        let start = Instant::now();
        let mut repaint = Repaint::new(start);
        assert!(repaint.ready(start));
        repaint.rendered(start);
        for tick in 1..=300 {
            let now = start + IDLE_POLL * tick;
            assert!(!repaint.ready(now));
            assert_eq!(repaint.wait(now), IDLE_POLL);
        }
        repaint.request();
        repaint.request();
        assert!(!repaint.ready(start + Duration::from_millis(1)));
        assert!(repaint.ready(start + FRAME_INTERVAL));
        repaint.rendered(start + FRAME_INTERVAL);
        assert!(!repaint.ready(start + FRAME_INTERVAL * 2));
    }
    #[test]
    fn busy_streams_are_limited_to_sixty_frames_per_second() {
        let start = Instant::now();
        let mut repaint = Repaint::new(start);
        let mut frames = 0;
        for millisecond in 0..1000 {
            let now = start + Duration::from_millis(millisecond);
            repaint.request();
            if repaint.ready(now) {
                repaint.rendered(now);
                frames += 1;
            }
        }
        assert!((55..=60).contains(&frames), "frames: {frames}");
    }
    #[test]
    fn clicks_and_resize_can_refresh_mouse_geometry_immediately() {
        let now = Instant::now();
        let mut repaint = Repaint::new(now);
        repaint.rendered(now);
        let click = now + Duration::from_millis(1);
        repaint.urgent(click);
        assert!(repaint.ready(click));
        assert_eq!(repaint.wait(click), Duration::ZERO);
    }
}
