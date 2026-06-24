use crate::error::AppError;
use argon2::password_hash::{rand_core::OsRng, SaltString};
use argon2::{Algorithm, Argon2, Params, PasswordHash, PasswordHasher, PasswordVerifier, Version};

pub fn hash_password(password: &str, mem_kib: u32, iters: u32) -> Result<String, AppError> {
    let salt = SaltString::generate(&mut OsRng);
    let params = Params::new(mem_kib, iters, 1, None)
        .map_err(|e| AppError::Internal(format!("argon2 params: {}", e)))?;
    let argon = Argon2::new(Algorithm::Argon2id, Version::V0x13, params);
    let h = argon.hash_password(password.as_bytes(), &salt)?;
    Ok(h.to_string())
}

pub fn verify_password(password: &str, encoded: &str) -> Result<bool, AppError> {
    let parsed = PasswordHash::new(encoded)?;
    Ok(Argon2::default()
        .verify_password(password.as_bytes(), &parsed)
        .is_ok())
}

/// 对客户端 HWID hex 做服务端二次盐化，避免 DB dump 即得明文 HWID
pub fn salt_hwid(hwid_hex: &str, server_salt: &[u8]) -> String {
    use sha2::{Digest, Sha256};
    let mut h = Sha256::new();
    h.update(server_salt);
    h.update(hwid_hex.as_bytes());
    hex::encode(h.finalize())
}
