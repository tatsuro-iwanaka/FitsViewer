GCC = g++ --std=c++2a -O3

MACOS_MIN = 14.0
TARGET = FitsViewer
HOMEBREW_PREFIX = /opt/homebrew
CSPICE_DIR = $(HOMEBREW_PREFIX)/Cellar/cspice/67

CFLAGS = -I$(CSPICE_DIR)/include -I. -I./imgui -I./imgui/backends `pkg-config --cflags glfw3` -DGL_SILENCE_DEPRECATION -mmacosx-version-min=$(MACOS_MIN)
LIBS = $(CSPICE_DIR)/lib/libcspice.a \
       $(HOMEBREW_PREFIX)/lib/libglfw3.a \
       -framework OpenGL -framework Cocoa -framework IOKit -framework CoreVideo

IMGUI_SRCS = imgui/imgui.cpp imgui/imgui_draw.cpp imgui/imgui_widgets.cpp \
             imgui/imgui_tables.cpp imgui/backends/imgui_impl_glfw.cpp \
             imgui/backends/imgui_impl_opengl3.cpp

SRCS = main.cpp $(IMGUI_SRCS)

APP_NAME = FitsViewer
APP_DIR = $(APP_NAME).app
CONTENTS = $(APP_DIR)/Contents
MACOS = $(CONTENTS)/MacOS
RESOURCES = $(CONTENTS)/Resources

all: $(TARGET)

$(TARGET): $(SRCS)
	$(GCC) $(CFLAGS) $(SRCS) $(LIBS) -mmacosx-version-min=$(MACOS_MIN) -o $(TARGET)

bundle: $(TARGET)
	@mkdir -p $(MACOS) $(RESOURCES)
	@cp $(TARGET) $(MACOS)/
	
	@vtool -set-build-version macos 11.0 15.0 -output $(MACOS)/$(TARGET).tmp $(MACOS)/$(TARGET)
	@mv $(MACOS)/$(TARGET).tmp $(MACOS)/$(TARGET)
	@chmod +x $(MACOS)/$(TARGET)
	
	@if [ -f "icon.icns" ]; then cp icon.icns $(RESOURCES)/; fi
	@if [ -d "kernels" ]; then cp -R kernels $(RESOURCES)/; fi
	
	# 静的なファイルをコピーして、構文チェックを実行
	@cp Info.plist $(CONTENTS)/Info.plist
	@plutil -lint $(CONTENTS)/Info.plist || (echo "Error: Info.plist is invalid" && exit 1)
	
	@xattr -rc $(APP_DIR)
	@codesign --force --deep --sign - $(APP_DIR)
	@echo "Bundle created with verified Info.plist."

clean:
	rm -rf $(TARGET) $(APP_DIR)