use std::io::Result;

fn main() -> Result<()> {
    let proto_root = std::path::PathBuf::from("../../../src/proto");
    let proto_file = proto_root.join("subscription.proto");

    println!("cargo:rerun-if-changed={}", proto_file.display());

    let mut cfg = prost_build::Config::new();
    cfg.out_dir("src/generated");
    std::fs::create_dir_all("src/generated").ok();
    cfg.compile_protos(&[proto_file], &[proto_root])?;
    Ok(())
}
