from pathlib import Path
import argparse, struct, hashlib, re
from cryptography.hazmat.primitives import serialization, hashes
from cryptography.hazmat.primitives.asymmetric import ec

def bundle(image: bytes, version: str, build: int, key) -> bytes:
    assert 0 < len(image) <= 580 * 1024 and image[0] == 0xE9
    assert len(version.encode()) < 32 and isinstance(build, int) and build > 0
    # M2SRCP1 header: magic(8), layout(1), chip(13 = ESP32-C6), image_bytes, build, version(32), sha256(32)
    header = struct.pack(
        "<8sIIII32s32s",
        b"M2SRCP1\x00",
        1,
        13,
        len(image),
        build,
        version.encode(),
        hashlib.sha256(image).digest(),
    )
    sig = key.sign(header, ec.ECDSA(hashes.SHA256()))
    assert len(sig) <= 80
    return header + struct.pack("<H80s", len(sig), sig) + image

if __name__ == "__main__":
    p = argparse.ArgumentParser(description="Sign ESP32-C6 RCP 2xOTA image (.m2s)")
    p.add_argument("image", help="Compiled ESP32-C6 esp_ot_rcp.bin")
    p.add_argument("output", help="Output .m2s path")
    p.add_argument("--version-header", default=str(Path(__file__).resolve().parents[1] / "firmware-c6-rcp-ota/main/rcp_ota_version.h"))
    p.add_argument("--key", required=True, help="ECDSA P-256 private signing key PEM")
    args = p.parse_args()

    vh = Path(args.version_header).read_text(encoding="utf-8")
    ver = re.search(r'RCP_OTA_VERSION\s+"([^"]+)"', vh).group(1)
    bld = int(re.search(r"RCP_OTA_BUILD\s+(\d+)", vh).group(1))
    data = Path(args.image).read_bytes()
    key = serialization.load_pem_private_key(Path(args.key).read_bytes(), password=None)
    result = bundle(data, ver, bld, key)
    out = Path(args.output)
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_bytes(result)
    print(f"Signed C6 RCP OTA package: {out.name} version={ver} build={bld} bytes={len(result)} SHA256={hashlib.sha256(result).hexdigest()}")
