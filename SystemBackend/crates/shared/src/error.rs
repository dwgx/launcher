use thiserror::Error;

#[derive(Debug, Error)]
pub enum AppError {
    #[error("invalid credentials")]
    InvalidCredentials,

    #[error("session expired")]
    SessionExpired,

    #[error("subscription expired")]
    SubscriptionExpired,

    #[error("hwid mismatch: account already bound to another machine")]
    HwidMismatch,

    #[error("database error: {0}")]
    Database(String),

    #[error("internal: {0}")]
    Internal(String),

    #[error("not found")]
    NotFound,
}

impl From<sqlx::Error> for AppError {
    fn from(e: sqlx::Error) -> Self {
        AppError::Database(e.to_string())
    }
}

impl From<argon2::password_hash::Error> for AppError {
    fn from(e: argon2::password_hash::Error) -> Self {
        AppError::Internal(format!("argon2: {}", e))
    }
}
