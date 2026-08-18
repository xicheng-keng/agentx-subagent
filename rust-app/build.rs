//! Compiles `../proto/agentx_ipc.proto` (shared with the C subagent's nanopb
//! codegen) into Rust types via `prost-build`. See docs/design.md ch.4 and
//! `proto/agentx_ipc.proto` for the wire contract.

fn main() {
    let proto_file = "../proto/agentx_ipc.proto";
    let proto_dir = "../proto";

    println!("cargo:rerun-if-changed={proto_file}");

    prost_build::compile_protos(&[proto_file], &[proto_dir])
        .expect("failed to compile proto/agentx_ipc.proto");
}
