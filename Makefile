BUILDDIR := builddir

ifeq ($(shell uname),Darwin)
  NATIVE_FILE := meson-native-macos.ini
  NATIVE_ARGS := --native-file $(NATIVE_FILE)
  CLANG_TIDY_EXTRA := -extra-arg=-stdlib=libc++ -extra-arg=-isysroot$(shell xcrun --show-sdk-path)
  ifeq ($(shell xcode-select -p 2>/dev/null),)
    $(error Xcode Command Line Tools required. Install with: xcode-select --install)
  endif
else
  CLANG_TIDY_EXTRA :=
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
SIZEDIR := builddir-size
COVDIR := builddir-cov

.PHONY: deps setup build build-minimal build-static build-size size-compare run test coverage coverage-summary lint clean clear-memory

deps:
ifeq ($(shell uname),Darwin)
	@command -v brew >/dev/null || { echo "Error: Homebrew is required. Install from https://brew.sh"; exit 1; }
	brew install meson llvm gcovr sqlite3
else
	sudo apt-get update
	sudo apt-get install -y g++ meson ninja-build libssl-dev libsqlite3-dev clang-tidy lld gcovr
endif

setup:
	@if [ ! -d $(BUILDDIR) ]; then meson setup $(BUILDDIR) $(NATIVE_ARGS) -Dcatch2:tests=false; fi

build: setup
	meson compile -C $(BUILDDIR)

build-minimal:
	@if [ ! -d $(MINDIR) ]; then meson setup $(MINDIR) $(NATIVE_ARGS) -Dcatch2:tests=false \
		-Dwith_anthropic=false -Dwith_ollama=false -Dwith_openrouter=false -Dwith_compatible=false \
		-Dwith_whatsapp=false -Dwith_sqlite_memory=false; fi
	meson compile -C $(MINDIR)

build-static:
	@if [ ! -d $(STATICDIR) ]; then meson setup $(STATICDIR) $(NATIVE_ARGS) -Ddefault_library=static -Dprefer_static=true -Dcatch2:tests=false; fi
	meson compile -C $(STATICDIR)

# Linux-focused size profile: optimize for size + section GC + stripped binary.
build-size:
	@if [ "$(shell uname)" = "Darwin" ]; then echo "build-size is intended for Linux"; exit 1; fi
	@if [ ! -d $(SIZEDIR) ]; then meson setup $(SIZEDIR) $(NATIVE_ARGS) -Dcatch2:tests=false \
		-Dbuildtype=minsize -Db_lto=true \
		-Dcpp_args='-ffunction-sections -fdata-sections -fvisibility=hidden' \
		-Dcpp_link_args='-Wl,--gc-sections -Wl,--strip-all'; fi
	meson compile -C $(SIZEDIR)
	strip --strip-unneeded $(SIZEDIR)/ptrclaw || true

size-compare: build-static build-size
	@echo "== Binary size comparison =="
	@ls -lh $(STATICDIR)/ptrclaw $(SIZEDIR)/ptrclaw
	@echo ""
	@python3 -c "from pathlib import Path; s=Path('builddir-static/ptrclaw').stat().st_size; m=Path('builddir-size/ptrclaw').stat().st_size; d=s-m; p=(d/s*100.0 if s else 0.0); print(f'static: {s} bytes'); print(f'size-optimized: {m} bytes'); print(f'saved: {d} bytes ({p:.2f}%)')"

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
	rm -f ~/.ptrclaw/memory.json ~/.ptrclaw/memory.db
	@echo "Memory cleared"

clean:
	rm -rf $(BUILDDIR) $(STATICDIR) $(MINDIR) $(SIZEDIR) $(COVDIR)
