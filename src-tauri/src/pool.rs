//! A bounded pool for the media scheme handlers (Phase 3.6). A request used to start a thread of its own, so a scrub or a
//! Timeline seek that asks for many byte ranges started many threads at once. Here the requests queue behind a few workers.

use std::sync::{mpsc, Arc, Mutex, OnceLock};

type Job = Box<dyn FnOnce() + Send + 'static>;

/// Four workers: enough that a seek and a thumbnail do not wait on each other, few enough that a scrub never starts a crowd.
const WORKERS: usize = 4;

static SENDER: OnceLock<Mutex<mpsc::Sender<Job>>> = OnceLock::new();

/// Runs `job` on one of the pool's workers, in the order the requests arrived.
pub fn run(job: impl FnOnce() + Send + 'static) {
    let sender = SENDER.get_or_init(|| {
        let (sender, receiver) = mpsc::channel::<Job>();
        let receiver = Arc::new(Mutex::new(receiver));
        for _ in 0..WORKERS {
            let receiver = receiver.clone();
            std::thread::spawn(move || loop {
                // The lock is released as soon as a job is taken, so the other workers wait only for the hand-over.
                let next = match receiver.lock() {
                    Ok(guard) => guard.recv(),
                    Err(_) => break,
                };
                match next {
                    Ok(job) => job(),
                    Err(_) => break,
                }
            });
        }
        Mutex::new(sender)
    });
    if let Ok(sender) = sender.lock() {
        let _ = sender.send(Box::new(job));
    }
}
