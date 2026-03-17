GCC = g++ --std=c++2a -O3
SWIFTC = swiftc

MACOS_MIN = 14.0
HOMEBREW_PREFIX = /opt/homebrew

# バイナリ名
BACKEND_TARGET = backend
FRONTEND_TARGET = FitsViewer

# C++ 設定
IMGUI_DIR = ./imgui
IMGUI_SRCS = $(IMGUI_DIR)/imgui.cpp \
             $(IMGUI_DIR)/imgui_draw.cpp \
             $(IMGUI_DIR)/imgui_widgets.cpp \
             $(IMGUI_DIR)/imgui_tables.cpp \
             $(IMGUI_DIR)/backends/imgui_impl_glfw.cpp \
             $(IMGUI_DIR)/backends/imgui_impl_opengl3.cpp

SRCS = main.cpp $(IMGUI_SRCS)
CFLAGS = -I. -I$(IMGUI_DIR) -I$(IMGUI_DIR)/backends `pkg-config --cflags glfw3` -DGL_SILENCE_DEPRECATION -mmacosx-version-min=$(MACOS_MIN)
LIBS = $(HOMEBREW_PREFIX)/lib/libglfw3.a \
       -framework OpenGL -framework Cocoa -framework IOKit -framework CoreVideo \
       -framework AppKit -framework Foundation

# App 構造定義
APP_NAME = FitsViewer
APP_DIR = $(APP_NAME).app
CONTENTS = $(APP_DIR)/Contents
MACOS = $(CONTENTS)/MacOS
RESOURCES = $(CONTENTS)/Resources

all: $(FRONTEND_TARGET) $(BACKEND_TARGET)

# 1. C++ バックエンドのビルド
$(BACKEND_TARGET): $(SRCS)
	$(GCC) $(CFLAGS) $(SRCS) $(LIBS) -o $(BACKEND_TARGET)

# 2. Swift フロントエンドのビルド
$(FRONTEND_TARGET): FitsViewer.swift
	$(SWIFTC) -sdk `xcrun --show-sdk-path` -target arm64-apple-macosx$(MACOS_MIN) FitsViewer.swift -o $(FRONTEND_TARGET)

# 3. バンドル作成
bundle: all
	@mkdir -p $(MACOS) $(RESOURCES)
	# 両方のバイナリを MacOS フォルダへコピー
	@cp $(FRONTEND_TARGET) $(MACOS)/
	@cp $(BACKEND_TARGET) $(MACOS)/
	@chmod +x $(MACOS)/$(FRONTEND_TARGET)
	@chmod +x $(MACOS)/$(BACKEND_TARGET)
	# 各種リソースのコピー
	@if [ -f "icon.icns" ]; then cp icon.icns $(RESOURCES)/; fi
	@if [ -d "kernels" ]; then cp -R kernels $(RESOURCES)/; fi
	@cp Info.plist $(CONTENTS)/Info.plist
	@plutil -lint $(CONTENTS)/Info.plist
	@xattr -rc $(APP_DIR)
	@codesign --force --deep --sign - $(APP_DIR)
	@echo "Bundle created with Swift frontend and C++ backend."

clean:
	rm -rf $(BACKEND_TARGET) $(FRONTEND_TARGET) $(APP_DIR)