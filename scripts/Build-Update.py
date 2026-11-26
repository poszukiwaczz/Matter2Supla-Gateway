from pathlib import Path
import argparse,struct,hashlib
from cryptography.hazmat.primitives import serialization,hashes
from cryptography.hazmat.primitives.asymmetric import ec
# Signs an S3 controller app image into an M2SOTA3 package.
# Version and build come from scripts/Build-Release.ps1, which derives them
# from version.h and the selected variant (standard / singleusb).
def bundle(image,version,build,key):
 assert 0<len(image)<=4*1024*1024 and image[0]==0xe9
 assert len(version.encode())<32 and type(build)is int and build>0
 header=struct.pack('<8sIIII32s32s',b'M2SOTA3',3,9,len(image),build,version.encode(),hashlib.sha256(image).digest())
 sig=key.sign(header,ec.ECDSA(hashes.SHA256()));assert len(sig)<=80
 return header+struct.pack('<H80s',len(sig),sig)+image
if __name__=='__main__':
 p=argparse.ArgumentParser()
 p.add_argument('image');p.add_argument('output')
 p.add_argument('--version',required=True);p.add_argument('--build',required=True,type=int)
 p.add_argument('--key',required=True,help='ECDSA P-256 private signing key PEM (kept outside this repository)')
 args=p.parse_args();out=Path(args.output);assert not out.exists(),'Preserve existing signed artifact'
 data=Path(args.image).read_bytes();assert args.version.encode()+b'\0' in data,'Image does not contain the declared firmware version string'
 key=serialization.load_pem_private_key(Path(args.key).read_bytes(),password=None);result=bundle(data,args.version,args.build,key);out.write_bytes(result)
 print('Signed S3 update package:',out.name,'bytes',len(result),'SHA256',hashlib.sha256(result).hexdigest())
