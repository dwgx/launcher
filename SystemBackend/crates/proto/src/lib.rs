// 由 build.rs 在编译时生成 src/generated/launcher.proto.rs
pub mod launcher_proto {
    include!(concat!("generated/", "launcher.proto.rs"));
}

pub use launcher_proto::*;
