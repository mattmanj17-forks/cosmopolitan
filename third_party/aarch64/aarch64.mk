#-*-mode:makefile-gmake;indent-tabs-mode:t;tab-width:8;coding:utf-8-*-┐
#───vi: set et ft=make ts=8 tw=8 fenc=utf-8 :vi───────────────────────┘

PKGS += THIRD_PARTY_AARCH64
THIRD_PARTY_AARCH64_HDRS = $(filter %.h,$(THIRD_PARTY_AARCH64_FILES))
THIRD_PARTY_AARCH64_FILES := $(wildcard third_party/aarch64/*)
