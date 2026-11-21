#pragma once
namespace gateway {
// HTTP diagnostics may omit optional stack data, but must never wait for CHIP.
template<class Manager>class TryStackLock {
 Manager &manager_;bool owned_;
public:
 explicit TryStackLock(Manager &m):manager_(m),owned_(m.TryLockChipStack()){}
 ~TryStackLock(){if(owned_)manager_.UnlockChipStack();}
 bool owned()const{return owned_;}
 TryStackLock(const TryStackLock&)=delete;
 TryStackLock&operator=(const TryStackLock&)=delete;
};
}
