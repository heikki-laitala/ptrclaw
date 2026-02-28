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
    -Dcpp_link_args='-Wl,--gc-sections -Wl,--strip-all'
  STRIP_CMD = strip --strip-unneeded $1
  ifeq ($(shell command -v clang++ >/dev/null 2>&1; echo $$?),0)
    NATIVE_FILE := meson-native-linux.ini
    NATIVE_ARGS := --native-file $(NATIVE_FILE)
  else
    NATIVE_FILE :=
    NATIVE_ARGS :=
  endif
endif

STATICDIR := builddir-static
MINDIR := builddir-minimal
COVDIR := builddir-cov
PIPEDIR := builddir-pipe
EMBDIR := builddir-emb

.PHONY: deps setup build build-emb build-minimal build-static run test coverage coverage-summary lint clean clear-memory memory-clean

deps:
ifeq ($(shell uname),Darwin)
	@command -v brew >/dev/null || { echo "Error: Homebrew is required. Install from https://brew.sh"; exit 1; }
	brew install meson llvm gcovr sqlite3
else
	sudo apt-get update
	sudo apt-get install -y g++ clang meson ninja-build libssl-dev libsqlite3-dev clang-tidy lld gcovr
endif

setup:
	@if [ ! -d $(BUILDDIR) ]; then meson setup $(BUILDDIR) $(NATIVE_ARGS) -Dcatch2:tests=false; fi

build: setup
	meson compile -C $(BUILDDIR)

build-emb:
	@if [ ! -d $(EMBDIR) ]; then meson setup $(EMBDIR) $(NATIVE_ARGS) -Dcatch2:tests=false -Dwith_embeddings=true; fi
	meson compile -C $(EMBDIR)

build-minimal:
	@if [ ! -d $(MINDIR) ]; then meson setup $(MINDIR) $(NATIVE_ARGS) -Dcatch2:tests=false \
		-Dwith_anthropic=false -Dwith_ollama=false -Dwith_openrouter=false -Dwith_compatible=false \
		-Dwith_whatsapp=false -Dwith_sqlite_memory=false $(SIZE_FLAGS); fi
	meson compile -C $(MINDIR) ptrclaw
	$(call STRIP_CMD,$(MINDIR)/ptrclaw) 2>/dev/null || true

build-static:
	@if [ ! -d $(STATICDIR) ]; then meson setup $(STATICDIR) $(NATIVE_ARGS) -Ddefault_library=static -Dprefer_static=true -Dcatch2:tests=false $(SIZE_FLAGS); fi
	meson compile -C $(STATICDIR) ptrclaw
	$(call STRIP_CMD,$(STATICDIR)/ptrclaw) 2>/dev/null || true

run: build
	./$(BUILDDIR)/ptrclaw

test: build
	meson test -C $(BUILDDIR)

coverage:
	@if [ ! -d $(COVDIR) ]; then meson setup $(COVDIR) $(NATIVE_ARGS) -Db_coverage=true -Dcatch2:tests=false; fi
	meson test -C $(COVDIR)
	gcovr --root . $(COVDIR) --filter src/ --html-details $(COVDIR)/coverage.html
	@echo "Coverage report: $(COVDIR)/coverage.html"

coverage-summary:
	@if [ ! -d $(COVDIR) ]; then echo "Run 'make coverage' first"; exit 1; fi
	gcovr --root . $(COVDIR) --filter src/

lint: build
	run-clang-tidy -quiet -p $(BUILDDIR) $(CLANG_TIDY_EXTRA) '^(?!.*subprojects).*(src|tests)/' 2>&1 | grep -v 'warnings generated'

clear-memory:
	rm -f ~/.ptrclaw/memory.json ~/.ptrclaw/memory.db ~/.ptrclaw/memory.db-shm ~/.ptrclaw/memory.db-wal
	@echo "Memory cleared"

memory-clean: clear-memory

clean:
	rm -rf $(BUILDDIR) $(STATICDIR) $(MINDIR) $(COVDIR) $(PIPEDIR) $(EMBDIR)
