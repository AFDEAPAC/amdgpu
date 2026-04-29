# V17.4 Driver DKMS Build From Source

Source tree:
`amdgpu-6.14.14-2212064.el8.v17_3_no_guard`

This is the 6.14.14 hotfix driver source used with the V17.4 runtime delivery. The DKMS package version remains `6.14.14-2212064.el8.v17_3_no_guard` because the V17.4 tail fix was in ROCr/CLR, while the driver source is the validated 6.14.14 RDMA/failed-pin hotfix.

Expected kernel on hjbog22:
`5.10.134-13.1.al8.x86_64`

## Install Build Dependencies

```bash
sudo yum install -y dkms gcc make elfutils-libelf-devel kernel-devel-$(uname -r) kernel-headers-$(uname -r)
# or dnf:
# sudo dnf install -y dkms gcc make elfutils-libelf-devel kernel-devel-$(uname -r) kernel-headers-$(uname -r)
```

## Unpack Source

```bash
tar -xzf driver-v17.4-6.14.14-dkms-source.tar.gz
cd driver-v17.4-6.14.14-dkms-source
```

## Driver Load Options

```bash
sudo tee /etc/modprobe.d/amd.conf >/dev/null <<'MODPROBE'
options amdkcl suballoc_timeout_ms=4000
options amdgpu rdma_pin_debug=1 gtt_lock_timeout_ms=4000 dmabuf_pin_max_mb=4096 gtt_multi_window=32
MODPROBE
```

## DKMS Build And Install

```bash
PKG_NAME=amdgpu
PKG_VER=6.14.14-2212064.el8.v17_3_no_guard
SRC_DIR=$PWD/amdgpu-6.14.14-2212064.el8.v17_3_no_guard

sudo rm -rf /usr/src/${PKG_NAME}-${PKG_VER}
sudo cp -a "$SRC_DIR" /usr/src/${PKG_NAME}-${PKG_VER}

sudo dkms remove -m ${PKG_NAME} -v ${PKG_VER} --all || true
sudo dkms add -m ${PKG_NAME} -v ${PKG_VER}
sudo dkms build -m ${PKG_NAME} -v ${PKG_VER} -k $(uname -r)
sudo dkms install -m ${PKG_NAME} -v ${PKG_VER} -k $(uname -r)

sudo depmod -a $(uname -r)
sudo reboot
```

## Verify After Reboot

```bash
uname -r
modinfo amdgpu | egrep 'filename|version|vermagic|srcversion'
rocm-smi
```

Expected:
- `version: 6.14.14`
- `vermagic` matches the running kernel, e.g. `5.10.134-13.1.al8.x86_64`
- `/etc/modprobe.d/amd.conf` contains `dmabuf_pin_max_mb=4096` and `gtt_lock_timeout_ms=4000`.

## Rollback

```bash
sudo dkms remove -m amdgpu -v 6.14.14-2212064.el8.v17_3_no_guard --all
sudo depmod -a
sudo reboot
```
