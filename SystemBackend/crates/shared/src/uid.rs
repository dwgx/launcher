// 7 位数字 UID（不前导 0）：1000000 ~ 9999999，900 万个空间。
// 用于公开展示，不参与 lookup（lookup 用 users.id UUID）。

use rand::{rngs::OsRng, Rng};

pub fn generate() -> String {
    let mut rng = OsRng;
    let n: u32 = rng.gen_range(1_000_000..10_000_000);
    n.to_string()
}

pub fn is_valid(s: &str) -> bool {
    s.len() == 7 && s.bytes().all(|b| b.is_ascii_digit()) && s.bytes().next() != Some(b'0')
}

#[cfg(test)]
mod t {
    use super::*;
    #[test]
    fn generated_is_valid() {
        for _ in 0..50 {
            assert!(is_valid(&generate()));
        }
    }
    #[test]
    fn rejects_short() {
        assert!(!is_valid("123"));
    }
    #[test]
    fn rejects_zero_prefix() {
        assert!(!is_valid("0123456"));
    }
    #[test]
    fn rejects_letters() {
        assert!(!is_valid("abc1234"));
    }
}
