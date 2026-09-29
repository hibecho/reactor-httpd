# Keep the historical test-directory entry points; all rules live in ../Makefile.
.DEFAULT_GOAL := help
ROOT := $(abspath $(dir $(lastword $(MAKEFILE_LIST)))/..)
.PHONY: help
help:
	+$(MAKE) -C "$(ROOT)" help
%:
	+$(MAKE) -C "$(ROOT)" $@
