from pathlib import Path
import argparse,struct,hashlib,re
from cryptography.hazmat.primitives import serialization,hashes
from cryptography.hazmat.primitives.asymmetric import ec
root=Path(__file__).resolve().parents[1]
def bundle(image,version,build,key):
 assert 0<len(image)<=4*1024*1024 and image[0]==0xe9
 assert len(version.encode())<32 and type(build)is int and build>0
 header=struct.pack('<8sIIII32s32s',b'M2SOTA3',3,9,len(image),build,version.encode(),hashlib.sha256(image).digest())
 sig=key.sign(header,ec.ECDSA(hashes.SHA256()));assert len(sig)<=80
 return header+struct.pack('<H80s',len(sig),sig)+image
if __name__=='__main__':
 p=argparse.ArgumentParser();p.add_argument('image');p.add_argument('output');p.add_argument('--key',default=str(root/'private/update-signing-key.pem'));args=p.parse_args();out=Path(args.output);assert not out.exists(),'Preserve existing signed artifact'
 version=(root/'firmware/controller/main/version.h').read_text();name=re.search(r'FW_VERSION "([^"]+)"',version).group(1);number=int(re.search(r'FW_BUILD (\d+)',version).group(1));data=Path(args.image).read_bytes();assert name.encode() in data,'Image does not match the declared source version'
 key=serialization.load_pem_private_key(Path(args.key).read_bytes(),password=None);result=bundle(data,name,number,key);out.write_bytes(result);print('Signed S3 update package:',out.name,'bytes',len(result),'SHA256',hashlib.sha256(result).hexdigest());print('Private signing key is not included. Same or lower build numbers are rejected by the gateway.')
