// 短公开 UID 生成器：8 位 Crockford-base32（避开 I/L/O/U 易混字符）
//
// 例：K8RX2QZP / N7M4DB8C
// 32^8 ≈ 1.1e12，足够。冲突时重试。

use rand::{Rng, rngs::OsRng};

const ALPHABET: &[u8; 32] = b"0123456789ABCDEFGHJKMNPQRSTVWXYZ";

pub fn generate() -> String {
    let mut rng = OsRng;
    let mut s = String::with_capacity(8);
    for _ in 0..8 {
        let b = ALPHABET[rng.gen_range(0..32)];
        s.push(b as char);
    }
    s
}

pub fn is_valid(s: &str) -> bool {
    s.len() == 8 && s.chars().all(|c|
        c.is_ascii_alphanumeric() && c != 'I' && c != 'L' && c != 'O' && c != 'U')
}
