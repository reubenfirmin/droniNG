# Export the committed droniNG raster assets from their SVG sources.
MAGICK ?= magick
ARTWORK := ../artwork/Logo
ICON := $(ARTWORK)/droning_logo_only.svg
OSD := $(ARTWORK)/droning_osd.svg
ICONS := $(addprefix gcs_logo_,32 64 128 256)
ICONS := $(addsuffix .png,$(ICONS))
ANDROID := ../androidgcs/res
OSD_DIR := ../flight/Modules/OnScreenDisplay/inc
WELCOME := ../ground/gcs/share/welcome/images/welcome-logo.png
ASSETS := $(ICONS) gcs_nontrans_128.png gcs_splashscreen.png $(WELCOME) \
          $(ANDROID)/drawable-mdpi/ic_logo.png \
          $(ANDROID)/drawable-hdpi/ic_logo.png \
          $(ANDROID)/drawable-xhdpi/ic_logo.png \
          $(OSD_DIR)/droning.png $(OSD_DIR)/droningbig.png

.PHONY: all osd
all: $(ASSETS)

gcs_logo_%.png: $(ICON)
	$(MAGICK) -background none -density 192 $< -resize $*x$* PNG32:$@

gcs_nontrans_128.png: $(ICON)
	$(MAGICK) -background '#142532' -density 192 $< -resize 128x128 -alpha remove PNG32:$@

gcs_splashscreen.png: $(ARTWORK)/gcs_splash.svg
	$(MAGICK) -background none -density 192 $< -resize 384x320 PNG32:$@

$(WELCOME): $(ARTWORK)/welcome-logo.svg
	$(MAGICK) -background none -density 192 $< -resize 256x256 PNG32:$@

$(ANDROID)/drawable-mdpi/ic_logo.png: $(ICON)
	$(MAGICK) -background none -density 192 $< -resize 48x48 PNG32:$@

$(ANDROID)/drawable-hdpi/ic_logo.png: $(ICON)
	$(MAGICK) -background none -density 192 $< -resize 72x72 PNG32:$@

$(ANDROID)/drawable-xhdpi/ic_logo.png: $(ICON)
	$(MAGICK) -background none -density 192 $< -resize 96x96 PNG32:$@

$(OSD_DIR)/droning.png: $(OSD)
	$(MAGICK) -background none -density 192 $< -resize 109x109 -gravity center -extent 120x109 PNG32:$@

$(OSD_DIR)/droningbig.png: $(OSD)
	$(MAGICK) -background none -density 192 $< -resize 158x158 -gravity center -extent 173x158 PNG32:$@

osd: $(OSD_DIR)/droning.png $(OSD_DIR)/droningbig.png
	cd $(OSD_DIR) && MPLBACKEND=Agg ./encode_images.sh
