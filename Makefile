BUILDDIR := builddir

ifeq ($(shell uname),Darwin)
  NATIVE_FILE := meson-native-macos.ini
  CLANG_TIDY_EXTRA := -extra-arg=-stdlib=libc++ -extra-arg=-isysroot$(shell xcrun --show-sdk-path)
  ifeq ($(shell xcode-select -p 2>/dev/null),)
    $(error Xcode Command Line Tools required. Install with: xcode-select --install)
  endif
else
  NATIVE_FILE := meson-native-linux.ini
  CLANG_TIDY_EXTRA :=
endif

STATICDIR := builddir-static
MINDIR := builddir-minimal
COVDIR := builddir-cov

.PHONY: deps setup build build-minimal build-static run test coverage coverage-summary lint clean

deps:
ifeq ($(shell uname),Darwin)
	@command -v brew >/dev/null || { echo "Error: Homebrew is required. Install from https://brew.sh"; exit 1; }
	brew install meson llvm gcovr
else
	sudo apt-get update
	sudo apt-get install -y g++ meson ninja-build libssl-dev libcurl4-openssl-dev clang-tidy lld gcovr
endif

setup:
	@if [ ! -d $(BUILDDIR) ]; then meson setup $(BUILDDIR) --native-file $(NATIVE_FILE) -Dcatch2:tests=false; fi

build: setup
	meson compile -C $(BUILDDIR)

build-minimal:
	@if [ ! -d $(MINDIR) ]; then meson setup $(MINDIR) --native-file $(NATIVE_FILE) -Dcatch2:tests=false \
		-Dwith_anthropic=false -Dwith_ollama=false -Dwith_openrouter=false -Dwith_compatible=false \
		-Dwith_whatsapp=false; fi
	meson compile -C $(MINDIR)

build-static:
	@if [ ! -d $(STATICDIR) ]; then meson setup $(STATICDIR) --native-file $(NATIVE_FILE) -Ddefault_library=static -Dprefer_static=true -Dcatch2:tests=false; fi
	meson compile -C $(STATICDIR)

run: build
	./$(BUILDDIR)/ptrclaw

test: build
	meson test -C $(BUILDDIR)

coverage:
	@if [ ! -d $(COVDIR) ]; then meson setup $(COVDIR) --native-file $(NATIVE_FILE) -Db_coverage=true -Dcatch2:tests=false; fi
	meson test -C $(COVDIR)
	gcovr --root . $(COVDIR) --filter src/ --html-details $(COVDIR)/coverage.html
	@echo "Coverage report: $(COVDIR)/coverage.html"

coverage-summary:
	@if [ ! -d $(COVDIR) ]; then echo "Run 'make coverage' first"; exit 1; fi
	gcovr --root . $(COVDIR) --filter src/

lint: build
	run-clang-tidy -quiet -p $(BUILDDIR) $(CLANG_TIDY_EXTRA) '^(?!.*subprojects).*(src|tests)/' 2>&1 | grep -v 'warnings generated'

clean:
	rm -rf $(BUILDDIR) $(STATICDIR) $(MINDIR) $(COVDIR)
