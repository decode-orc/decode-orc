/*
 * File:        orc_preview_views.h
 * Module:      orc-view-types
 * Purpose:     Shared preview-view contracts for registry-driven preview tools.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#pragma once

#include <orc/stage/node_id.h>
#include <orc/stage/preview/orc_preview_types.h>
#include <orc/stage/preview/orc_rendering.h>
#include <orc/stage/preview/orc_vectorscope.h>

#include <optional>
#include <string>
#include <vector>

#include "orc_histogram.h"

namespace orc {

/**
 * @brief Data payload category produced by a preview view request.
 */
enum class PreviewViewPayloadKind {
  None,
  Image,
  Vectorscope,
  Histogram,
};

/**
 * @brief Lightweight descriptor for a registered preview view.
 */
struct PreviewViewDescriptor {
  std::string id;            ///< Stable identifier used for routing
  std::string display_name;  ///< Human-readable view name for UI/CLI
  std::vector<VideoDataType> supported_data_types;
};

/**
 * @brief Which scopes a preview render should produce alongside the image.
 *
 * The scope dialogues plot the frame the preview is showing, so their payloads
 * come from the carrier the render already decoded rather than from a second
 * decode of the same frame. What the render cannot know is which dialogues are
 * open and how their line selection is set, so the GUI states that here and
 * the worker answers it in the same pass.
 */
struct PreviewScopeRequest {
  /// Produce a vectorscope payload.
  bool want_vectorscope{false};
  /// Produce a histogram payload.
  bool want_histogram{false};
  /// Registered view answering the vectorscope. Carried because the dialogue
  /// chooses it, and the worker must ask for the same one the synchronous
  /// path would have.
  std::string vectorscope_view_id;
  /// Domain being previewed. This, not a caller's choice, decides which
  /// vectorscope acquisition runs: a colour-domain type has decoder planes to
  /// plot, a signal-domain one has a carrier to demodulate.
  VideoDataType data_type{VideoDataType::CompositeNTSC};
  /// The frame and line selection to plot, exactly as the dialogue built it.
  PreviewCoordinate coordinate;

  /// True when neither scope was asked for, so no carrier need be fetched.
  bool wantsNothing() const { return !want_vectorscope && !want_histogram; }

  bool operator==(const PreviewScopeRequest& other) const {
    return want_vectorscope == other.want_vectorscope &&
           want_histogram == other.want_histogram &&
           vectorscope_view_id == other.vectorscope_view_id &&
           data_type == other.data_type && coordinate == other.coordinate;
  }
  bool operator!=(const PreviewScopeRequest& other) const {
    return !(*this == other);
  }
};

/**
 * @brief Scope payloads produced from one carrier fetch.
 *
 * A payload is disengaged either because it was not asked for or because the
 * carrier yielded nothing for it; the two cases are indistinguishable to the
 * caller by design, since both mean "nothing to plot".
 */
struct PreviewScopePayloads {
  std::optional<VectorscopeData> vectorscope;
  std::optional<VideoHistogramData> histogram;

  bool empty() const {
    return !vectorscope.has_value() && !histogram.has_value();
  }
};

/**
 * @brief Result payload returned by preview-view request_data().
 */
struct PreviewViewDataResult {
  bool success{false};
  std::string error_message;
  PreviewViewPayloadKind payload_kind{PreviewViewPayloadKind::None};
  std::optional<PreviewImage> image;
  std::optional<VectorscopeData> vectorscope;
  std::optional<VideoHistogramData> histogram;

  bool is_valid() const {
    if (!success) {
      return false;
    }

    if (payload_kind == PreviewViewPayloadKind::Image) {
      return image.has_value() && image->is_valid();
    }

    if (payload_kind == PreviewViewPayloadKind::Vectorscope) {
      return vectorscope.has_value();
    }

    if (payload_kind == PreviewViewPayloadKind::Histogram) {
      return histogram.has_value();
    }

    return false;
  }
};

/**
 * @brief Result of preview-view export operation.
 */
struct PreviewViewExportResult {
  bool success{false};
  std::string error_message;
};

// =============================================================================
// Preview View Contract
// =============================================================================

/**
 * @brief Preview view contract used by the core registry.
 *
 * Implementations are bound to a specific node at construction time. The
 * request_data interface intentionally only accepts data type + coordinate so
 * callers can use a uniform contract across GUI and CLI.
 */
class IPreviewView {
 public:
  virtual ~IPreviewView() = default;

  virtual std::vector<VideoDataType> supported_data_types() const = 0;

  virtual PreviewViewDataResult request_data(
      VideoDataType data_type, const PreviewCoordinate& coordinate) = 0;

  virtual PreviewViewExportResult export_as(const std::string& format,
                                            const std::string& path) const = 0;
};

}  // namespace orc
