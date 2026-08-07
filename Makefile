SHELL := /bin/bash
BUILDDIR := builddir

ifeq ($(shell uname),Darwin)
  NATIVE_FILE := meson-native-macos.ini
  NATIVE_ARGS := --native-file $(NATIVE_FILE)
  CLANG_TIDY_EXTRA := -extra-arg=-stdlib=libc++ -extra-arg=-isysroot$(shell xcrun --show-sdk-path)
  SIZE_FLAGS :=
  STRIP_CMD = strip -x $1
  ifeq ($(shell xcode-select -p 2>/dev/null),)
    $(error Xcode Command Line Tools required. Install with: xcode-select --install)
  endif
else
  CLANG_TIDY_EXTRA :=
  SIZE_FLAGS := -Dcpp_args='-ffunction-sections -fdata-sections -fvisibility=hidden' \
    -Dcpp_link_args='-fuse-ld=lld -Wl,--gc-sections -Wl,--icf=all -Wl,--strip-all'
  STRIP_CMD = strip --strip-unneeded $1
  ifeq ($(shell command -v clang++ >/dev/null 2>&1; echo $$?),0)
    NATIVE_FILE := meson-native-linux.ini
    NATIVE_ARGS := --native-file $(NATIVE_FILE)
  else
    NATIVE_FILE :=
    NATIVE_ARGS :=
  endif
endif

# Distribution builds optimize for size, on every platform. meson's `release` buildtype
# means -O3, which inlines for speed and is worth ~40% of this binary; b_ndebug drops
# assert() bodies from libc++ and the subprojects (src/ has none of its own). The flags
# above are the per-platform half — section GC and stripping, which Darwin's linker
# spells differently.
SIZE_FLAGS += -Doptimization=s -Db_ndebug=true

# Reconfigure a build directory that already exists instead of reusing it as-is. These
# targets carry option flags, and a directory created before a flag changed would keep the
# old ones silently — a smaller binary that is only smaller on a clean checkout.
setup_size = @if [ -d $(1) ]; then meson setup --reconfigure $(1) $(NATIVE_ARGS) $(2); \
	else meson setup $(1) $(NATIVE_ARGS) $(2); fi

STATICDIR := builddir-static
MINDIR := builddir-minimal
SERVEDIR := builddir-serving
COVDIR := builddir-cov
PIPEDIR := builddir-pipe
EMBDIR := builddir-emb
SDKDIR := builddir-sdk

.PHONY: deps setup build build-emb build-minimal build-static build-sdk build-pipe build-serving test-serving run test coverage coverage-summary lint clean clear-memory memory-clean

deps:
ifeq ($(shell uname),Darwin)
	@command -v brew >/dev/null || { echo "Error: Homebrew is required. Install from https://brew.sh"; exit 1; }
	brew install meson llvm gcovr sqlite3 mbedtls
else
	sudo apt-get update
	sudo apt-get install -y g++ clang meson ninja-build pkg-config libssl-dev libmbedtls-dev libsqlite3-dev clang-tidy lld gcovr
endif

# Extra options for the default build directory. CI sets it to widen the lint database:
# clang-tidy only ever sees files the build compiles, so a feature left off is a file that
# is never linted. Empty for a local `make build`, which then behaves exactly as before —
# including not reconfiguring a directory that already exists.
MESON_SETUP_EXTRA ?=

setup:
	@if [ ! -d $(BUILDDIR) ]; then \
		meson setup $(BUILDDIR) $(NATIVE_ARGS) -Dcatch2:tests=false $(MESON_SETUP_EXTRA); \
	elif [ -n "$(MESON_SETUP_EXTRA)" ]; then \
		meson setup --reconfigure $(BUILDDIR) $(NATIVE_ARGS) -Dcatch2:tests=false \
			$(MESON_SETUP_EXTRA); \
	fi

build: setup
	meson compile -C $(BUILDDIR)

build-emb:
	@if [ ! -d $(EMBDIR) ]; then meson setup $(EMBDIR) $(NATIVE_ARGS) -Dcatch2:tests=false -Dwith_embeddings=true; fi
	meson compile -C $(EMBDIR)

MINIMAL_OPTS := -Dcatch2:tests=false \
	-Dwith_anthropic=false -Dwith_ollama=false -Dwith_openrouter=false -Dwith_compatible=false \
	-Dwith_whatsapp=false -Dwith_sqlite_memory=false -Dwith_embeddings=false $(SIZE_FLAGS)

build-minimal:
	$(call setup_size,$(MINDIR),$(MINIMAL_OPTS))
	meson compile -C $(MINDIR) ptrclaw
	$(call STRIP_CMD,$(MINDIR)/ptrclaw) 2>/dev/null || true

# Multi-session serving pod: workspace-scoped file tools, no shell and no cron. The tool
# flags are not optional decoration — meson refuses with_serving alongside them, because
# both register a tool named file_read.
# Providers: OpenAI only, over chat completions and the subscription OAuth flow. The pod is
# a fixed deployment rather than a general-purpose binary, so the four it cannot reach are
# compiled out. Config's default provider follows the build (src/config.hpp), so a pod
# config that omits "provider" still starts.
#
# The baseline is not overridable. Replacing it wholesale is the trap: a partial override
# would drop the remaining false flags, and those options default to true in meson, so
# asking for one provider would quietly compile in every provider.
SERVING_PROVIDER_BASE := -Dwith_anthropic=false -Dwith_ollama=false \
	-Dwith_openrouter=false -Dwith_compatible=false

# Appended after the baseline instead. meson takes the last value for a repeated option, so
# this re-enables exactly what it names and leaves the rest off. Overriding is necessary
# rather than optional: these flags are re-applied on every reconfigure, so a `meson
# configure` by hand would be undone by the next `make build-serving`.
#
#   make build-serving SERVING_PROVIDERS="-Dwith_anthropic=true"
#
SERVING_PROVIDERS ?=

SERVING_OPTS := -Dcatch2:tests=false \
	-Dwith_serving=true -Dwith_tools=false -Dwith_file_read=false \
	-Dwith_http=true -Dwith_telegram=false -Dwith_whatsapp=false \
	$(SERVING_PROVIDER_BASE) $(SERVING_PROVIDERS) $(SIZE_FLAGS)

build-serving:
	$(call setup_size,$(SERVEDIR),$(SERVING_OPTS))
	meson compile -C $(SERVEDIR) ptrclaw
	$(call STRIP_CMD,$(SERVEDIR)/ptrclaw) 2>/dev/null || true

# The profile's own assertions — which tools are absent — only hold in this configuration,
# so they need a build of their own rather than a filter on the default suite.
SERVING_TEST_OPTS := -Dwith_serving=true -Dwith_tools=false -Dwith_file_read=false \
	-Dwith_http=true $(SERVING_PROVIDER_BASE) $(SERVING_PROVIDERS)

test-serving:
	$(call setup_size,$(SERVEDIR)-test,$(SERVING_TEST_OPTS))
	meson test -C $(SERVEDIR)-test --print-errorlogs

STATIC_OPTS := -Ddefault_library=static -Dprefer_static=true -Dcatch2:tests=false \
	-Dwith_mbedtls=true $(SIZE_FLAGS)

build-static:
	$(call setup_size,$(STATICDIR),$(STATIC_OPTS))
	meson compile -C $(STATICDIR) ptrclaw
	$(call STRIP_CMD,$(STATICDIR)/ptrclaw) 2>/dev/null || true

SDK_OPTS := -Dcatch2:tests=false \
	-Dwith_embed=true -Dwith_telegram=false -Dwith_whatsapp=false \
	-Dwith_ollama=false -Dwith_tools=false $(SIZE_FLAGS)

build-sdk:
	$(call setup_size,$(SDKDIR),$(SDK_OPTS))
	meson compile -C $(SDKDIR) ptrclaw_shared
	@for f in $(SDKDIR)/libptrclaw_shared.dylib $(SDKDIR)/libptrclaw_shared.so; do \
		[ -f "$$f" ] && $(call STRIP_CMD,$$f) 2>/dev/null || true; \
	done

build-pipe:
	@if [ ! -d $(PIPEDIR) ]; then meson setup $(PIPEDIR) $(NATIVE_ARGS) -Dcatch2:tests=false -Dwith_pipe=true; fi
	meson compile -C $(PIPEDIR)

run: build
	./$(BUILDDIR)/ptrclaw

test: build
	meson test -C $(BUILDDIR) --print-errorlogs

coverage:
	@if [ ! -d $(COVDIR) ]; then meson setup $(COVDIR) $(NATIVE_ARGS) -Db_coverage=true -Dcatch2:tests=false; fi
	meson test -C $(COVDIR)
	gcovr --root . $(COVDIR) --filter src/ --html-details $(COVDIR)/coverage.html
	@echo "Coverage report: $(COVDIR)/coverage.html"

coverage-summary:
	@if [ ! -d $(COVDIR) ]; then echo "Run 'make coverage' first"; exit 1; fi
	gcovr --root . $(COVDIR) --filter src/

lint: build
	@set -o pipefail; run-clang-tidy -quiet -p $(BUILDDIR) -warnings-as-errors='*' $(CLANG_TIDY_EXTRA) '^(?!.*subprojects).*(src|tests)/' 2>&1 | grep -v 'warnings generated'

clear-memory:
	rm -f ~/.ptrclaw/memory.json ~/.ptrclaw/memory.db ~/.ptrclaw/memory.db-shm ~/.ptrclaw/memory.db-wal
	rm -rf ~/.ptrclaw/tee
	@echo "Memory cleared"

memory-clean: clear-memory

clean:
	rm -rf $(BUILDDIR) $(STATICDIR) $(MINDIR) $(COVDIR) $(PIPEDIR) $(EMBDIR) $(SDKDIR) $(SERVEDIR) $(SERVEDIR)-test
