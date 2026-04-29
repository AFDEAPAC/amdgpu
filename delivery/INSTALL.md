# V17.4.1 amdgpu DKMS install (offline)

## Source
GitHub: https://github.com/AFDEAPAC/amdgpu  branch v17.4-no-guard, tag v17.4.1
Patch on top of V17.4: 0002-amdgpu-kfd-fix-rdma-hang-p0-p2.patch

## Files in this package
| file | sha256 | role |
|---|---|---|
| amdgpu.ko, amdttm.ko, amdkcl.ko, amd-sched.ko, amddrm_ttm_helper.ko, amddrm_buddy.ko, amddrm_exec.ko, amdxcp.ko | see SHA256SUMS | signed kernel modules for 5.10.134-13.1.al8.x86_64 |
| driver-v17.4.1-6.14.14-dkms-source.tar.gz | see SHA256SUMS | DKMS source tarball (PACKAGE_VERSION=6.14.14-2212064.el8.v17_4_1_rdma_fix) |
| 0002-amdgpu-kfd-fix-rdma-hang-p0-p2.patch | see SHA256SUMS | unified diff vs V17.4 baseline |

## Install (DKMS, kernel == 5.10.134-13.1.al8.x86_64)
```bash
sudo tar -xzf driver-v17.4.1-6.14.14-dkms-source.tar.gz -C /usr/src
sudo dkms add -m amdgpu -v 6.14.14-2212064.el8.v17_4_1_rdma_fix
sudo dkms build  -m amdgpu -v 6.14.14-2212064.el8.v17_4_1_rdma_fix
sudo dkms install -m amdgpu -v 6.14.14-2212064.el8.v17_4_1_rdma_fix
```

## Install (.ko hot-swap, same kernel only)
```bash
# All running GPU work will die when amdgpu unloads.
sudo systemctl stop tuned                           # if any
sudo modprobe -r amdgpu amdttm amdkcl amd-sched amddrm_buddy amddrm_exec amdxcp amddrm_ttm_helper
sudo cp *.ko /lib/modules/$(uname -r)/extra/
sudo depmod -a
sudo modprobe amdgpu
```

## Patches included
1. P0-prereq amdgpu_kfd_rdma_v155.c     orphan reaper takes RDMA quota ownership
2. P2       amdgpu_amdkfd_gpuvm.c       early-return SKIP when reaper already force-unpinned
3 (P0)      kfd_events.c                 (a) signal queue-less processes in detect-hang
                                         (b) cap WaitRelaxed sleep + reset poll (2 s)
                                         (c) absolute INFINITE deadline (30 s)

