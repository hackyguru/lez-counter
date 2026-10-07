//! A counter everyone shares.
//!
//! The number lives in this program's own shard of one account — the public
//! PDA derived from the program's address and the seed `COUNTER_SEED`, so
//! every client can find it without being told. Anyone may increment it: a
//! program can always write its own shard, and this one doesn't ask the
//! account for authorization.
//!
//! Instruction: the amount to add (1..=100), Borsh `u64`.
//! Shard data:  the total, little-endian `u64` (empty means 0).

use lee_core::program::{Plan, PlanInput, run_program};

type Instruction = u64;

/// The step a single call may add. Keeps one caller from jumping the count to
/// u64::MAX and ending the fun for everyone.
const MAX_STEP: u64 = 100;

fn main() {
    run_program(plan, apply)
}

#[expect(clippy::needless_pass_by_value, reason = "run_program hands the instruction over by value")]
fn plan(input: &PlanInput, by: Instruction) -> Plan {
    assert!((1..=MAX_STEP).contains(&by), "Increment must be between 1 and {MAX_STEP}");
    let [counter] = input.accounts.as_slice() else {
        panic!("Increment takes exactly one account: the counter");
    };
    let mut plan = Plan::new(input);
    plan.effect(counter, &by);
    plan
}

#[expect(clippy::unnecessary_wraps, reason = "run_program's apply returns None to keep a shard")]
fn apply(by: u64, pre: &[u8]) -> Option<Vec<u8>> {
    let current = match pre.len() {
        0 => 0,
        8 => u64::from_le_bytes(pre.try_into().expect("8 bytes")),
        n => panic!("Counter shard holds {n} bytes, expected 8"),
    };
    let next = current.checked_add(by).expect("Counter overflow");
    Some(next.to_le_bytes().to_vec())
}
