//! Bound parsing work between opportunities to service keyboard/mouse input.
use std::time::{Duration, Instant};

pub(crate) const MAX_BYTES: usize = 256 * 1024;
const MAX_EVENTS: usize = 64;
const MAX_TIME: Duration = Duration::from_millis(6);

pub(crate) struct Budget {
    deadline: Instant,
    bytes: usize,
    events: usize,
}
impl Budget {
    pub(crate) fn new() -> Self {
        Self {
            deadline: Instant::now() + MAX_TIME,
            bytes: 0,
            events: 0,
        }
    }
    pub(crate) fn available(&self) -> bool {
        self.bytes < MAX_BYTES && self.events < MAX_EVENTS && Instant::now() < self.deadline
    }
    pub(crate) fn record(&mut self, bytes: usize) {
        self.events += 1;
        self.bytes += bytes;
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn parsing_budget_bounds_large_frames_tiny_events_and_elapsed_time() {
        let mut budget = Budget::new();
        budget.deadline += Duration::from_secs(1);
        for _ in 0..MAX_BYTES / crate::protocol::MAX_PAYLOAD {
            assert!(budget.available());
            budget.record(crate::protocol::MAX_PAYLOAD);
        }
        assert!(!budget.available());
        let mut budget = Budget::new();
        budget.deadline += Duration::from_secs(1);
        for _ in 0..MAX_EVENTS {
            assert!(budget.available());
            budget.record(0);
        }
        assert!(!budget.available());
        let budget = Budget {
            deadline: Instant::now(),
            bytes: 0,
            events: 0,
        };
        assert!(!budget.available());
    }
}
