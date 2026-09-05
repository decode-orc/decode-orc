/*
 * File:        ffmpegpresetdialog.cpp
 * Module:      orc-gui
 * Purpose:     Configuration dialog for FFmpeg video sink presets
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2025-2026 Simon Inns
 */

#include "ffmpegpresetdialog.h"

#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QProcess>
#include <QRegularExpression>
#include <QTextEdit>
#include <QVBoxLayout>
#include <map>
#include <set>

#include "logging.h"

FFmpegPresetDialog::FFmpegPresetDialog(const QString& project_path,
                                       QWidget* parent)
    : ConfigDialogBase("FFmpeg Export Preset Configuration", parent),
      updating_ui_(false),
      project_path_(project_path) {
  // Increase minimum height to accommodate all sections
  setMinimumHeight(800);

  // Initialize preset database
  all_presets_ = {
      // Lossless/Archive
      {"mkv-ffv1", "FFV1 Lossless",
       "Best for archival storage. Mathematically lossless compression. Large "
       "file size but perfect quality preservation. Use for master copies.",
       "mkv", "ffv1", false, false, 0, "medium", 0},
      {"mkv-ffv1-bt601", "FFV1 for VP415e",
       "Player-ready lossless FFV1 on the ITU-R BT.601 13.5 MHz sampling grid "
       "rather than the 4fsc grid: 720 pixels wide, band-limited (Lanczos), "
       "with no change to the line count. The whole 720-sample window is "
       "taken from source - nothing is cropped and the margins carry the "
       "source's own blanking, not synthetic black. 8-bit 4:2:2, 16 slices, "
       "every frame a keyframe, interlacing untouched, audio included, and "
       "the disc's VBI metadata - picture numbers, time codes and chapters - "
       "embedded in the file, so no sidecar is needed. "
       "Intended for players driving real BT.601 hardware - such as the "
       "Philips VP415 LaserDisc player emulator - which would otherwise "
       "resample every frame at play time. The 4fsc FFV1 export remains the "
       "master; this is a derived file.",
       "mkv", "ffv1-bt601", false, false, 0, "medium", 0},

      // ProRes (Professional)
      {"mov-prores", "ProRes 422 HQ",
       "Professional editing codec. Excellent quality, moderate file size. "
       "Standard for professional video editing. Compatible with Final Cut "
       "Pro, DaVinci Resolve, Adobe Premiere.",
       "mov", "prores", false, false, 0, "medium", 0},
      {"mov-prores_4444", "ProRes 4444",
       "ProRes with alpha channel support and highest chroma quality. Use when "
       "you need the best possible quality for compositing or color grading.",
       "mov", "prores_4444", false, false, 0, "medium", 0},
      {"mov-prores_4444xq", "ProRes 4444 XQ",
       "Highest quality ProRes variant. Maximum quality for demanding "
       "post-production workflows. Very large files.",
       "mov", "prores_4444xq", false, false, 0, "medium", 0},
      {"mov-prores_videotoolbox", "ProRes (Apple Hardware)",
       "Hardware-accelerated ProRes encoding on Apple Silicon and recent Intel "
       "Macs. Fast encoding with excellent quality.",
       "mov", "prores_videotoolbox", true, false, 0, "medium", 0},

      // Uncompressed
      {"mov-v210", "V210 (10-bit 4:2:2 Uncompressed)",
       "Completely uncompressed 10-bit 4:2:2 video. Massive file sizes but "
       "zero quality loss. Use for highest-quality mastering.",
       "mov", "v210", false, false, 0, "medium", 0},
      {"mov-v410", "V410 (10-bit 4:4:4 Uncompressed)",
       "Completely uncompressed 10-bit 4:4:4 video. Even larger than V210 but "
       "preserves all chroma information.",
       "mov", "v410", false, false, 0, "medium", 0},

      // Broadcast
      {"mxf-mpeg2video", "D10/IMX (Broadcast)",
       "Sony IMX/XDCAM D10 format for broadcast delivery. MXF container with "
       "MPEG-2 intra-frame encoding. Standard for broadcast archives.",
       "mxf", "mpeg2video", false, false, 0, "medium", 50000000},

      // H.264 (Universal Playback)
      {"mp4-h264", "H.264 (High Quality)",
       "Universal playback compatibility. Excellent quality-to-size ratio. "
       "Plays on virtually all devices and platforms. Good for archival and "
       "sharing.",
       "mp4", "h264", true, true, 18, "slow", 0},
      {"mp4-h264_lossless", "H.264 Lossless",
       "Mathematically lossless H.264 encoding. Smaller than FFV1 but slower "
       "to encode. Good compromise for archival.",
       "mp4", "h264", false, false, 0, "veryslow", 0},
      {"mov-h264", "H.264 in MOV",
       "H.264 in QuickTime MOV container. Better compatibility with Apple "
       "ecosystem and professional tools than MP4.",
       "mov", "h264", true, true, 18, "slow", 0},

      // H.265 (Better Compression)
      {"mp4-hevc", "H.265/HEVC (High Quality)",
       "Next-generation codec with 50% better compression than H.264. Smaller "
       "files, same quality. Requires modern devices for playback.",
       "mp4", "hevc", true, true, 23, "slow", 0},
      {"mp4-hevc_lossless", "H.265/HEVC Lossless",
       "Mathematically lossless H.265 encoding. Better compression than H.264 "
       "lossless. Excellent for archival with modern tools.",
       "mp4", "hevc", false, false, 0, "veryslow", 0},
      {"mov-hevc", "H.265/HEVC in MOV",
       "H.265 in QuickTime MOV container. Better compatibility with Apple "
       "ecosystem and professional tools.",
       "mov", "hevc", true, true, 23, "slow", 0},

      // AV1 (Modern)
      {"mp4-av1", "AV1 (Web Delivery)",
       "Modern royalty-free codec. Better compression than H.265. Excellent "
       "for web streaming. Limited device support currently.",
       "mp4", "av1", false, true, 32, "medium", 0},
      {"mp4-av1_lossless", "AV1 Lossless",
       "Mathematically lossless AV1 encoding. Best compression for lossless "
       "archival. Slow encoding but excellent results.",
       "mp4", "av1_lossless", false, false, 0, "medium", 0}};

  // Create category selection group
  auto* category_group = create_group("Export Category");
  auto* category_layout = qobject_cast<QFormLayout*>(category_group->layout());

  QStringList categories;
  categories << "Lossless/Archive"
             << "Professional/ProRes"
             << "Uncompressed"
             << "Broadcast"
             << "Universal (H.264)"
             << "Modern (H.265/AV1)"
             << "Hardware Accelerated";

  category_combo_ =
      add_combobox(category_layout, "Category:", categories,
                   "Select the export category that best matches your needs");

  // Create preset selection group
  auto* preset_group = create_group("Preset Selection");
  auto* preset_layout = qobject_cast<QFormLayout*>(preset_group->layout());

  preset_combo_ = add_combobox(preset_layout, "Preset:", QStringList(),
                               "Select the specific export preset");

  description_label_ = new QLabel();
  description_label_->setWordWrap(true);
  description_label_->setMinimumHeight(80);
  preset_layout->addRow("Description:", description_label_);

  // Output filename group
  auto* filename_group = create_group("Output Filename");
  auto* filename_layout = qobject_cast<QFormLayout*>(filename_group->layout());

  auto* filename_container = new QWidget();
  auto* filename_hlayout = new QHBoxLayout(filename_container);
  filename_hlayout->setContentsMargins(0, 0, 0, 0);

  filename_edit_ = new QLineEdit();
  filename_edit_->setPlaceholderText("output.mp4");
  browse_btn_ = new QPushButton("Browse...");

  filename_hlayout->addWidget(filename_edit_);
  filename_hlayout->addWidget(browse_btn_);

  filename_layout->addRow("Filename:", filename_container);
  add_info_label(filename_layout,
                 "Output filename with extension. Extension will automatically "
                 "update when you change the preset.");

  connect(browse_btn_, &QPushButton::clicked, this,
          &FFmpegPresetDialog::on_browse_filename_clicked);

  // Hardware encoder group (hidden by default)
  hardware_group_ = create_group("Hardware Acceleration");
  auto* hardware_layout = qobject_cast<QFormLayout*>(hardware_group_->layout());

  QStringList hw_options;
  hw_options << "Software (libx264/libx265)" << "Auto-detect hardware";
  hardware_encoder_combo_ =
      add_combobox(hardware_layout, "Encoder:", hw_options,
                   "Select hardware or software encoding");

  hardware_status_label_ = new QLabel();
  hardware_status_label_->setWordWrap(true);
  hardware_layout->addRow("Status:", hardware_status_label_);
  hardware_group_->setVisible(false);

  // Options group
  auto* options_group = create_group("Export Options");
  auto* options_layout = qobject_cast<QFormLayout*>(options_group->layout());

  deinterlace_checkbox_ =
      add_checkbox(options_layout, "Deinterlace for web",
                   "Apply deinterlacing filter (bwdif) for progressive web "
                   "playback. Recommended for H.264/H.265/AV1 web variants.");
  deinterlace_checkbox_->setChecked(false);

  embed_audio_checkbox_ = add_checkbox(
      options_layout, "Embed audio",
      "Include audio channel pairs from the source (if available)");
  embed_audio_checkbox_->setChecked(false);

  audio_gain_spinbox_ = add_double_spinbox(
      options_layout, "Audio gain (dB):", -24.0, 24.0, 0.0, 1,
      "Gain applied to the embedded audio in decibels. 0 = unchanged; "
      "positive values boost (6 dB roughly doubles the amplitude), negative "
      "values attenuate. Samples are clipped at full scale.");
  audio_gain_spinbox_->setEnabled(false);  // Follows the embed audio option

  embed_captions_checkbox_ = add_checkbox(
      options_layout, "Embed closed captions",
      "Convert EIA-608 closed captions to subtitle track (MP4/MOV only)");
  embed_captions_checkbox_->setChecked(false);

  embed_chapters_checkbox_ = add_checkbox(
      options_layout, "Embed chapter metadata",
      "Write chapter markers from VBI data to output file (MKV/MP4/MOV only)");
  embed_chapters_checkbox_->setChecked(false);

  embed_disc_metadata_checkbox_ = add_checkbox(
      options_layout, "Embed disc metadata",
      "Embed the LaserDisc VBI metadata - picture numbers, time codes, "
      "chapters, stop codes and programme status - as an attachment inside "
      "the file, so a player emulator needs no sidecar (MKV only)");
  embed_disc_metadata_checkbox_->setChecked(false);

  QStringList disc_detail_options;
  disc_detail_options << "Address map only" << "Include raw VBI";
  disc_metadata_detail_combo_ = add_combobox(
      options_layout, "Disc metadata detail:", disc_detail_options,
      "Address map only writes the picture-number map, events and disc "
      "status - a few kilobytes. Include raw VBI adds the raw biphase words "
      "for every field (about 2.3 MB for a full side) so codes decode-orc "
      "does not yet interpret stay recoverable.");
  disc_metadata_detail_combo_->setEnabled(false);  // Follows the option above

  QStringList aspect_options;
  aspect_options << "Auto (square pixels)" << "4:3 (SD television)"
                 << "16:9 (widescreen)";
  aspect_ratio_combo_ = add_combobox(
      options_layout, "Display aspect ratio:", aspect_options,
      "Display aspect ratio signalled to players. Metadata only - the video "
      "is not rescaled. Most SD material should be played back at 4:3.");

  video_filter_edit_ = new QLineEdit();
  video_filter_edit_->setPlaceholderText("e.g. fieldmatch,decimate");
  video_filter_edit_->setToolTip(
      "Optional custom FFmpeg video filter chain applied before encoding, "
      "using the same syntax as ffmpeg's -vf option. Examples: "
      "fieldmatch,decimate (inverse telecine), crop=692:554. Leave empty for "
      "no filtering. An invalid filter string causes the export to fail.");
  options_layout->addRow("Custom video filter:", video_filter_edit_);

  // Advanced settings group
  auto* advanced_group = create_group("Advanced Settings (Optional)");
  auto* advanced_layout = qobject_cast<QFormLayout*>(advanced_group->layout());

  add_info_label(advanced_layout,
                 "These settings override the preset defaults. Leave at "
                 "default unless you have specific requirements.");

  QStringList quality_presets;
  quality_presets << "Default (from preset)" << "Fast" << "Medium" << "Slow"
                  << "Very Slow";
  quality_preset_combo_ =
      add_combobox(advanced_layout, "Encoder Speed:", quality_presets,
                   "Encoder speed preset. Slower = better compression/quality "
                   "at same file size");

  crf_spinbox_ = add_spinbox(
      advanced_layout, "Quality (CRF):", 0, 51, 0,
      "Constant Rate Factor: lower = better quality, larger files. 0 = auto "
      "from preset, 18 = visually lossless, 23 = high quality, 28 = medium");

  bitrate_spinbox_ =
      add_spinbox(advanced_layout, "Bitrate (Mbps):", 0, 500, 0,
                  "Target bitrate in Mbps. 0 = use CRF mode (recommended). "
                  "Only needed for specific delivery requirements.");

  // Detect available hardware encoders
  detect_available_hardware_encoders();

  // Connect signals
  connect(category_combo_, QOverload<int>::of(&QComboBox::currentIndexChanged),
          this, &FFmpegPresetDialog::on_category_changed);
  connect(preset_combo_, QOverload<int>::of(&QComboBox::currentIndexChanged),
          this, &FFmpegPresetDialog::on_preset_changed);
  connect(hardware_encoder_combo_,
          QOverload<int>::of(&QComboBox::currentIndexChanged), this,
          &FFmpegPresetDialog::on_hardware_encoder_changed);
  connect(deinterlace_checkbox_, &QCheckBox::checkStateChanged, this,
          &FFmpegPresetDialog::on_deinterlace_changed);
  connect(embed_audio_checkbox_, &QCheckBox::toggled, audio_gain_spinbox_,
          &QWidget::setEnabled);
  connect(embed_disc_metadata_checkbox_, &QCheckBox::toggled,
          disc_metadata_detail_combo_, &QWidget::setEnabled);

  // Initialize with first category
  on_category_changed(0);
}

void FFmpegPresetDialog::apply_configuration() {
  if (current_category_presets_.empty() || preset_combo_->currentIndex() < 0) {
    return;
  }

  const auto& preset = current_category_presets_[preset_combo_->currentIndex()];

  // Set basic output format (container-codec), normalized to backend-supported
  // values. Some UI presets are profile/lossless variants that map to base
  // codecs plus extra params.
  std::string normalized_codec = preset.codec;
  if (normalized_codec == "h264_lossless") {
    normalized_codec = "h264";
  } else if (normalized_codec == "hevc_lossless") {
    normalized_codec = "hevc";
  } else if (normalized_codec == "av1_lossless") {
    normalized_codec = "av1";
  } else if (normalized_codec == "prores_4444" ||
             normalized_codec == "prores_4444xq" ||
             normalized_codec == "prores_videotoolbox") {
    normalized_codec = "prores";
  }

  std::string format_string = preset.container + "-" + normalized_codec;
  // Applying an FFmpeg preset always selects FFmpeg output on the merged
  // Video Sink stage, even if the node was previously in raw mode.
  set_parameter("output_mode", std::string("ffmpeg"));
  set_parameter("ffmpeg_format", format_string);

  // Set hardware encoder preference
  std::string hardware_encoder = "none";
  if (preset.codec == "prores_videotoolbox") {
    hardware_encoder = "videotoolbox";
  }
  if (preset.supports_hardware && hardware_group_->isVisible() &&
      hardware_encoder_combo_->currentIndex() > 0) {
    if (!available_hw_encoders_.empty()) {
      hardware_encoder =
          available_hw_encoders_[hardware_encoder_combo_->currentIndex() -
                                 1];  // -1 for "None" option
    }
  }
  set_parameter("hardware_encoder", hardware_encoder);

  // Set ProRes profile if applicable
  if (normalized_codec == "prores") {
    // ProRes profile comes from preset variant.
    std::string profile = "hq";
    if (preset.codec == "prores_4444") {
      profile = "4444";
    } else if (preset.codec == "prores_4444xq") {
      profile = "4444xq";
    }
    set_parameter("prores_profile", profile);
  }

  // Set lossless mode
  bool lossless = (preset.format_string.find("_lossless") != std::string::npos);
  set_parameter("use_lossless_mode", lossless);

  // Set deinterlacing
  set_parameter("apply_deinterlace", deinterlace_checkbox_->isChecked());

  // Set display aspect ratio (combo order matches these values)
  static const char* kAspectValues[] = {"auto", "4:3", "16:9"};
  int aspect_idx = aspect_ratio_combo_->currentIndex();
  if (aspect_idx < 0 || aspect_idx > 2) {
    aspect_idx = 0;
  }
  set_parameter("display_aspect_ratio", std::string(kAspectValues[aspect_idx]));

  // Set custom video filter chain (empty clears any previous filter)
  set_parameter("video_filter",
                video_filter_edit_->text().trimmed().toStdString());

  // Set encoder preset
  std::string encoder_preset = preset.default_preset;
  if (quality_preset_combo_->currentIndex() > 0) {
    const char* presets[] = {"", "fast", "medium", "slow", "veryslow"};
    encoder_preset = presets[quality_preset_combo_->currentIndex()];
  }
  set_parameter("encoder_preset", encoder_preset);

  // Set CRF
  int crf = crf_spinbox_->value();
  if (crf == 0) {
    crf = preset.default_crf;
  }
  set_parameter("encoder_crf", crf);

  // Set bitrate
  int bitrate_mbps = bitrate_spinbox_->value();
  int bitrate =
      bitrate_mbps > 0 ? bitrate_mbps * 1000000 : preset.default_bitrate;
  set_parameter("encoder_bitrate", bitrate);

  // Set options
  set_parameter("embed_audio", embed_audio_checkbox_->isChecked());
  set_parameter("audio_gain_db", audio_gain_spinbox_->value());
  set_parameter("embed_closed_captions", embed_captions_checkbox_->isChecked());
  set_parameter("embed_chapter_metadata",
                embed_chapters_checkbox_->isChecked());
  set_parameter("embed_disc_metadata",
                embed_disc_metadata_checkbox_->isChecked());
  set_parameter(
      "disc_metadata_detail",
      std::string(disc_metadata_detail_combo_->currentIndex() == 1 ? "full"
                                                                   : "map"));

  // Set output filename (output_path parameter)
  QString filename = filename_edit_->text().trimmed();
  if (!filename.isEmpty()) {
    set_parameter("output_path", filename.toStdString());
  }
}

void FFmpegPresetDialog::load_from_parameters(
    const std::map<std::string, orc::ParameterValue>& params) {
  updating_ui_ = true;

  // Load output format and try to find matching preset
  auto it = params.find("ffmpeg_format");
  if (it != params.end() && std::holds_alternative<std::string>(it->second)) {
    const std::string& format = std::get<std::string>(it->second);

    // Find matching preset
    bool found = false;
    for (size_t i = 0; i < all_presets_.size(); ++i) {
      if (all_presets_[i].format_string == format) {
        // Find which category this preset belongs to
        // For simplicity, just select first category for now
        category_combo_->setCurrentIndex(0);
        update_preset_list();

        // Find preset in current category
        for (size_t j = 0; j < current_category_presets_.size(); ++j) {
          if (current_category_presets_[j].format_string == format) {
            preset_combo_->setCurrentIndex(static_cast<int>(j));
            found = true;
            break;
          }
        }
        break;
      }
    }

    if (!found) {
      category_combo_->setCurrentIndex(0);
      preset_combo_->setCurrentIndex(0);
    }
  }

  // Load encoder preset
  auto preset_it = params.find("encoder_preset");
  if (preset_it != params.end() &&
      std::holds_alternative<std::string>(preset_it->second)) {
    const std::string& preset = std::get<std::string>(preset_it->second);
    if (preset == "fast") {
      quality_preset_combo_->setCurrentIndex(1);
    } else if (preset == "medium") {
      quality_preset_combo_->setCurrentIndex(2);
    } else if (preset == "slow") {
      quality_preset_combo_->setCurrentIndex(3);
    } else if (preset == "veryslow") {
      quality_preset_combo_->setCurrentIndex(4);
    } else {
      quality_preset_combo_->setCurrentIndex(0);
    }
  }

  // Load CRF
  auto crf_it = params.find("encoder_crf");
  if (crf_it != params.end() && std::holds_alternative<int>(crf_it->second)) {
    crf_spinbox_->setValue(std::get<int>(crf_it->second));
  }

  // Load bitrate
  auto bitrate_it = params.find("encoder_bitrate");
  if (bitrate_it != params.end() &&
      std::holds_alternative<int>(bitrate_it->second)) {
    int bitrate = std::get<int>(bitrate_it->second);
    bitrate_spinbox_->setValue(bitrate / 1000000);  // Convert to Mbps
  }

  // Load options
  auto audio_it = params.find("embed_audio");
  if (audio_it != params.end() &&
      std::holds_alternative<bool>(audio_it->second)) {
    embed_audio_checkbox_->setChecked(std::get<bool>(audio_it->second));
  }
  // Gain follows the embed audio option (checkbox signal handles later edits)
  audio_gain_spinbox_->setEnabled(embed_audio_checkbox_->isChecked());

  auto gain_it = params.find("audio_gain_db");
  if (gain_it != params.end() &&
      std::holds_alternative<double>(gain_it->second)) {
    audio_gain_spinbox_->setValue(std::get<double>(gain_it->second));
  }

  auto captions_it = params.find("embed_closed_captions");
  if (captions_it != params.end() &&
      std::holds_alternative<bool>(captions_it->second)) {
    embed_captions_checkbox_->setChecked(std::get<bool>(captions_it->second));
  }

  auto chapters_it = params.find("embed_chapter_metadata");
  if (chapters_it != params.end() &&
      std::holds_alternative<bool>(chapters_it->second)) {
    embed_chapters_checkbox_->setChecked(std::get<bool>(chapters_it->second));
  }

  auto disc_it = params.find("embed_disc_metadata");
  if (disc_it != params.end() &&
      std::holds_alternative<bool>(disc_it->second)) {
    embed_disc_metadata_checkbox_->setChecked(std::get<bool>(disc_it->second));
  }
  disc_metadata_detail_combo_->setEnabled(
      embed_disc_metadata_checkbox_->isChecked());

  auto disc_detail_it = params.find("disc_metadata_detail");
  if (disc_detail_it != params.end() &&
      std::holds_alternative<std::string>(disc_detail_it->second)) {
    disc_metadata_detail_combo_->setCurrentIndex(
        std::get<std::string>(disc_detail_it->second) == "full" ? 1 : 0);
  }

  // Load deinterlace option
  auto deint_it = params.find("apply_deinterlace");
  if (deint_it != params.end() &&
      std::holds_alternative<bool>(deint_it->second)) {
    deinterlace_checkbox_->setChecked(std::get<bool>(deint_it->second));
  }

  // Load display aspect ratio
  auto aspect_it = params.find("display_aspect_ratio");
  if (aspect_it != params.end() &&
      std::holds_alternative<std::string>(aspect_it->second)) {
    const std::string& aspect = std::get<std::string>(aspect_it->second);
    if (aspect == "4:3") {
      aspect_ratio_combo_->setCurrentIndex(1);
    } else if (aspect == "16:9") {
      aspect_ratio_combo_->setCurrentIndex(2);
    } else {
      aspect_ratio_combo_->setCurrentIndex(0);
    }
  }

  // Load custom video filter
  auto vf_it = params.find("video_filter");
  if (vf_it != params.end() &&
      std::holds_alternative<std::string>(vf_it->second)) {
    video_filter_edit_->setText(
        QString::fromStdString(std::get<std::string>(vf_it->second)));
  }

  // Load output filename
  auto output_path_it = params.find("output_path");
  if (output_path_it != params.end() &&
      std::holds_alternative<std::string>(output_path_it->second)) {
    filename_edit_->setText(
        QString::fromStdString(std::get<std::string>(output_path_it->second)));
  }

  updating_ui_ = false;
}

void FFmpegPresetDialog::on_category_changed(int index) {
  if (updating_ui_) return;
  update_preset_list();
}

void FFmpegPresetDialog::on_preset_changed(int index) {
  if (updating_ui_) return;
  update_preset_description();

  // Show/hide hardware encoder group based on preset
  if (index >= 0 &&
      index < static_cast<int>(current_category_presets_.size())) {
    const auto& preset = current_category_presets_[index];
    hardware_group_->setVisible(preset.supports_hardware &&
                                !available_hw_encoders_.empty());
    deinterlace_checkbox_->setEnabled(preset.supports_deinterlace);
    if (!preset.supports_deinterlace) {
      // A disabled checkbox must not keep applying bwdif from whichever
      // preset was selected before.
      deinterlace_checkbox_->setChecked(false);
    }

    // Only Matroska carries attachments; MP4 and MOV reject them outright, so
    // a tick left over from an MKV preset must not survive the switch.
    const bool disc_metadata_supported = (preset.container == "mkv");
    embed_disc_metadata_checkbox_->setEnabled(disc_metadata_supported);
    if (!disc_metadata_supported) {
      embed_disc_metadata_checkbox_->setChecked(false);
    }

    if (preset.format_string == "mkv-ffv1-bt601") {
      // The VP415e preset exists to produce a complete playable side, so
      // audio comes along by default. Aspect goes back to Auto because the
      // export stamps the BT.601 sample aspect ratio itself - a whole-frame
      // 4:3 override would squeeze the picture into the blanking columns.
      embed_audio_checkbox_->setChecked(true);
      aspect_ratio_combo_->setCurrentIndex(0);

      // The point of this preset is a self-contained file for a player
      // emulator, which needs the picture-number map to seek at all. Chapter
      // markers come along too: they fall out of the same VBI pass and make
      // the file navigable in an ordinary player as well. Detail stays on the
      // address map - the raw VBI dump is an archival choice, not an
      // emulation one, and the preset should not quietly add megabytes.
      embed_disc_metadata_checkbox_->setChecked(true);
      embed_chapters_checkbox_->setChecked(true);
      disc_metadata_detail_combo_->setCurrentIndex(0);
    }

    // Auto-update file extension based on selected output format
    QString current_filename = filename_edit_->text();
    std::string new_extension =
        get_file_extension_for_format(preset.format_string);

    if (!current_filename.isEmpty()) {
      // Get the base filename without extension
      QFileInfo file_info(current_filename);
      QString base_name = file_info.completeBaseName();
      QString dir_path = file_info.path();

      // Construct new filename with updated extension
      QString new_filename;
      if (dir_path != ".") {
        new_filename =
            dir_path + "/" + base_name + QString::fromStdString(new_extension);
      } else {
        new_filename = base_name + QString::fromStdString(new_extension);
      }
      filename_edit_->setText(new_filename);
    } else {
      // Set default filename with appropriate extension
      filename_edit_->setText(QString("output") +
                              QString::fromStdString(new_extension));
    }
  }
}

void FFmpegPresetDialog::on_hardware_encoder_changed(int index) {
  if (updating_ui_) return;

  if (index == 0) {
    hardware_status_label_->setText(
        "Using software encoding (slower but compatible)");
  } else {
    if (!available_hw_encoders_.empty()) {
      hardware_status_label_->setText(
          QString("Using hardware encoder: %1 (faster)")
              .arg(QString::fromStdString(available_hw_encoders_[0])));
    }
  }
}

void FFmpegPresetDialog::on_deinterlace_changed(Qt::CheckState state) {
  // Could update description or show warning
  Q_UNUSED(state);
}

void FFmpegPresetDialog::update_preset_list() {
  updating_ui_ = true;

  current_category_presets_.clear();
  preset_combo_->clear();

  int category = category_combo_->currentIndex();

  // Filter presets by category
  for (const auto& preset : all_presets_) {
    bool include = false;

    switch (category) {
      case 0:  // Lossless/Archive
        include = (preset.codec == "ffv1" || preset.codec == "ffv1-bt601" ||
                   preset.format_string.find("lossless") != std::string::npos);
        break;
      case 1:  // Professional/ProRes
        include = (preset.codec.find("prores") != std::string::npos);
        break;
      case 2:  // Uncompressed
        include = (preset.codec == "v210" || preset.codec == "v410");
        break;
      case 3:  // Broadcast
        include = (preset.codec == "mpeg2video");
        break;
      case 4:  // Universal (H.264)
        include = (preset.codec == "h264" &&
                   preset.format_string.find("_lossless") == std::string::npos);
        break;
      case 5:  // Modern (H.265/AV1)
        include = (preset.codec == "hevc" || preset.codec == "av1" ||
                   preset.codec == "av1_lossless");
        break;
      case 6:  // Hardware Accelerated
        include = preset.supports_hardware;
        break;
      default:
        break;
    }

    if (include) {
      current_category_presets_.push_back(preset);
      preset_combo_->addItem(QString::fromStdString(preset.name));
    }
  }

  updating_ui_ = false;

  if (preset_combo_->count() > 0) {
    preset_combo_->setCurrentIndex(0);
    // Manually trigger preset update since signal may not fire if index was
    // already 0
    on_preset_changed(0);
  } else {
    // No presets available - hide hardware group
    hardware_group_->setVisible(false);
  }
}

void FFmpegPresetDialog::update_preset_description() {
  int index = preset_combo_->currentIndex();
  if (index >= 0 &&
      index < static_cast<int>(current_category_presets_.size())) {
    const auto& preset = current_category_presets_[index];
    description_label_->setText(QString::fromStdString(preset.description));
  } else {
    description_label_->setText("No preset selected");
  }
}

void FFmpegPresetDialog::detect_available_hardware_encoders() {
  ORC_LOG_DEBUG(
      "FFmpegPresetDialog: Probing for available hardware encoders...");

  // Probe FFmpeg for available encoders by calling ffmpeg -encoders
  QProcess ffmpeg;
  ffmpeg.start("ffmpeg", QStringList() << "-encoders");

  if (!ffmpeg.waitForStarted(3000)) {
    // FFmpeg not found or failed to start - fall back to platform heuristics
    ORC_LOG_WARN(
        "FFmpegPresetDialog: FFmpeg not available, using platform heuristics");
#ifdef __linux__
    available_hw_encoders_.push_back("vaapi");
    ORC_LOG_DEBUG("FFmpegPresetDialog: Added fallback encoder: vaapi");
#endif

#ifdef __APPLE__
    available_hw_encoders_.push_back("videotoolbox");
    ORC_LOG_DEBUG("FFmpegPresetDialog: Added fallback encoder: videotoolbox");
#endif
    return;
  }

  if (!ffmpeg.waitForFinished(5000)) {
    ORC_LOG_WARN("FFmpegPresetDialog: FFmpeg -encoders command timed out");
    ffmpeg.kill();
    return;
  }

  // Parse ffmpeg output to detect hardware encoders
  QString output = QString::fromUtf8(ffmpeg.readAllStandardOutput());
  ORC_LOG_DEBUG(
      "FFmpegPresetDialog: Successfully retrieved encoder list from FFmpeg");

  // Hardware encoder patterns to look for
  // Format: "V..... encoder_name    Description"
  // The first character is 'V' for video encoders
  struct HWEncoder {
    QString pattern;     // Regex pattern to match encoder name
    QString identifier;  // Internal identifier we use
  };

  std::vector<HWEncoder> hw_encoders = {
      // NVIDIA NVENC
      {"h264_nvenc", "nvenc"},
      {"hevc_nvenc", "nvenc"},

      // Intel QuickSync
      {"h264_qsv", "qsv"},
      {"hevc_qsv", "qsv"},

      // AMD AMF
      {"h264_amf", "amf"},
      {"hevc_amf", "amf"},

      // VA-API (Linux)
      {"h264_vaapi", "vaapi"},
      {"hevc_vaapi", "vaapi"},

      // Apple VideoToolbox
      {"h264_videotoolbox", "videotoolbox"},
      {"hevc_videotoolbox", "videotoolbox"},
      {"prores_videotoolbox", "videotoolbox"},
  };

  // Track which hardware encoder types we've found
  std::set<QString> found_encoders;
  std::map<QString, std::vector<QString>>
      found_encoder_details;  // Track which specific encoders were found

  // Parse each line looking for hardware encoders
  QStringList lines = output.split('\n');
  for (const QString& line : lines) {
    // Skip header lines and non-video encoders
    if (!line.contains(QRegularExpression("^\\s*V"))) {
      continue;
    }

    // Check each hardware encoder pattern
    for (const auto& hw : hw_encoders) {
      if (line.contains(hw.pattern)) {
        found_encoders.insert(hw.identifier);
        found_encoder_details[hw.identifier].push_back(hw.pattern);
      }
    }
  }

  // Log what we found
  if (found_encoders.empty()) {
    ORC_LOG_DEBUG("FFmpegPresetDialog: No hardware encoders detected");
  } else {
    ORC_LOG_DEBUG("FFmpegPresetDialog: Detected {} hardware encoder type(s)",
                  found_encoders.size());
    for (const auto& [type, encoders] : found_encoder_details) {
      QString encoder_list =
          QStringList(encoders.begin(), encoders.end()).join(", ");
      ORC_LOG_DEBUG("FFmpegPresetDialog:   {}: [{}]", type.toStdString(),
                    encoder_list.toStdString());
    }
  }

  // Convert set to vector (removes duplicates)
  for (const QString& encoder : found_encoders) {
    available_hw_encoders_.push_back(encoder.toStdString());
  }

  // If no hardware encoders found but we're on a platform that typically has
  // them, add platform defaults as fallback
  if (available_hw_encoders_.empty()) {
    ORC_LOG_DEBUG(
        "FFmpegPresetDialog: No hardware encoders found, adding platform "
        "defaults");
#ifdef __linux__
    available_hw_encoders_.push_back("vaapi");
    ORC_LOG_DEBUG("FFmpegPresetDialog:   Added platform default: vaapi");
#endif

#ifdef __APPLE__
    available_hw_encoders_.push_back("videotoolbox");
    ORC_LOG_DEBUG("FFmpegPresetDialog:   Added platform default: videotoolbox");
#endif
  }

  // Log final summary
  if (available_hw_encoders_.empty()) {
    ORC_LOG_INFO("FFmpegPresetDialog: No hardware encoders available");
  } else {
    ORC_LOG_INFO("FFmpegPresetDialog: {} hardware encoder type(s) available",
                 available_hw_encoders_.size());
  }
}

std::string FFmpegPresetDialog::get_file_extension_for_format(
    const std::string& format_string) const {
  // Extract container from format string (e.g., "mp4-h264" -> "mp4")
  size_t dash_pos = format_string.find('-');
  if (dash_pos != std::string::npos) {
    std::string container = format_string.substr(0, dash_pos);
    return "." + container;
  }

  // Fallback to .mp4 if format string is invalid
  return ".mp4";
}

void FFmpegPresetDialog::on_browse_filename_clicked() {
  // Get current filename or use default
  QString current_filename = filename_edit_->text();
  if (current_filename.isEmpty()) {
    // Get current preset to determine default extension
    int preset_idx = preset_combo_->currentIndex();
    if (preset_idx >= 0 &&
        preset_idx < static_cast<int>(current_category_presets_.size())) {
      const auto& preset = current_category_presets_[preset_idx];
      std::string ext = get_file_extension_for_format(preset.format_string);
      current_filename = QString("output") + QString::fromStdString(ext);
    } else {
      current_filename = "output.mp4";
    }
  }

  // Determine start directory
  QString start_dir = QDir::homePath();
  QFileInfo file_info(current_filename);
  if (!current_filename.isEmpty()) {
    if (file_info.exists() && file_info.dir().exists()) {
      start_dir = file_info.dir().absolutePath();
    } else if (!file_info.path().isEmpty() && file_info.path() != ".") {
      QFileInfo parent_info(file_info.absolutePath());
      if (parent_info.exists() && parent_info.isDir()) {
        start_dir = parent_info.absolutePath();
      }
    }
  }

  // Build file filter based on current preset
  QString filter = "All Files (*)";
  int preset_idx = preset_combo_->currentIndex();
  if (preset_idx >= 0 &&
      preset_idx < static_cast<int>(current_category_presets_.size())) {
    const auto& preset = current_category_presets_[preset_idx];
    std::string ext = get_file_extension_for_format(preset.format_string);
    QString ext_upper =
        QString::fromStdString(ext).mid(1).toUpper();  // Remove . and uppercase
    filter = ext_upper + " Files (*" + QString::fromStdString(ext) +
             ");;All Files (*)";
  }

  // Show save file dialog
  QString selected_file = QFileDialog::getSaveFileName(
      this, "Select Output Video File", start_dir + "/" + file_info.fileName(),
      filter);

  if (!selected_file.isEmpty()) {
    // Convert to relative path if we have a project path
    QString path_to_store = selected_file;
    if (!project_path_.isEmpty()) {
      QDir project_dir(QFileInfo(project_path_).absolutePath());
      path_to_store = project_dir.relativeFilePath(selected_file);
    }
    filename_edit_->setText(path_to_store);
  }
}
