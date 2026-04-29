
export OS_NAME=rhel
export OS_VERSION=8.6
subdir-ccflags-y += -DOS_NAME_RHEL
subdir-ccflags-y += -DOS_VERSION_MAJOR=8
subdir-ccflags-y += -DOS_VERSION_MINOR=6
subdir-ccflags-y += -DOS_NAME_RHEL_8_6
subdir-ccflags-y += -DOS_NAME_RHEL_8_X
subdir-ccflags-y += -DDRM_VER=5 -DDRM_PATCH=10 -DDRM_SUB="0"
export CONFIG_HSA_AMD_SVM_AMDKCL=y
subdir-ccflags-y += -DCONFIG_HSA_AMD_SVM_AMDKCL

export CONFIG_HSA_AMD_AMDKCL=y
subdir-ccflags-y += -DCONFIG_HSA_AMD_AMDKCL

export CONFIG_DRM_AMDGPU_CIK_AMDKCL=y
subdir-ccflags-y += -DCONFIG_DRM_AMDGPU_CIK_AMDKCL

export CONFIG_DRM_AMDGPU_SI_AMDKCL=y
subdir-ccflags-y += -DCONFIG_DRM_AMDGPU_SI_AMDKCL

export CONFIG_DRM_AMDGPU_USERPTR_AMDKCL=y
subdir-ccflags-y += -DCONFIG_DRM_AMDGPU_USERPTR_AMDKCL

export CONFIG_DRM_AMD_DC_AMDKCL=y
subdir-ccflags-y += -DCONFIG_DRM_AMD_DC_AMDKCL

