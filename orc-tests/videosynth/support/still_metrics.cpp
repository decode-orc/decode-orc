/*
 * File:        still_metrics.cpp
 * Module:      orc-videosynth-support
 * Purpose:     Source-referenced accuracy of a decoded still against its
 *              picture truth: component RMS, RGB PSNR, CIEDE2000, chroma
 *              vector error, cross-colour, cross-luminance and edge response
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 decode-orc contributors
 */

#include "still_metrics.h"

#include <algorithm>
#include <array>
#include <cmath>

namespace orc_videosynth {

namespace {

constexpr double kPi = 3.14159265358979323846;

// The colour-difference weights the video sink decodes with
// (outputwriter.cpp): U = kB (B'-Y'), V = kR (R'-Y').
constexpr double kB = 0.49211104112248356308804691718185;
constexpr double kR = 0.87728321993817866838972487283129;

constexpr double kPalSubcarrierHz = 4433618.75;

// Thresholds, in ComponentPicture units (fractions of black to white).
constexpr double kChromaPresent = 0.05;
constexpr double kChromaAbsent = 0.005;
constexpr double kLumaDetail = 0.02;
constexpr double kChromaEdge = 0.05;

// Long enough that the band edges' transitions (about 0.75 MHz wide at
// 4fsc) stay clear of the subcarrier.
constexpr int kFilterTaps = 129;

struct Rgb {
  double r = 0.0;
  double g = 0.0;
  double b = 0.0;
};

Rgb yuv_to_rgb(double y, double u, double v) {
  const double r = y + (v / kR);
  const double b = y + (u / kB);
  const double g = (y - (0.299 * r) - (0.114 * b)) / 0.587;
  return {r, g, b};
}

// A Blackman-windowed linear-phase FIR passing [low_hz, high_hz); a
// low_hz of 0 makes it a low-pass, and a high_hz at or past Nyquist a
// high-pass.
std::vector<double> band_filter(double low_hz, double high_hz,
                                double sample_rate_hz) {
  std::vector<double> taps(kFilterTaps, 0.0);
  const int centre = kFilterTaps / 2;
  const double nyquist = sample_rate_hz / 2.0;
  const double lo = std::clamp(low_hz, 0.0, nyquist) / sample_rate_hz;
  const double hi = std::clamp(high_hz, 0.0, nyquist) / sample_rate_hz;
  for (int n = 0; n < kFilterTaps; ++n) {
    const double m = n - centre;
    auto sinc_lp = [m](double fc) {
      return m == 0 ? 2.0 * fc : std::sin(2.0 * kPi * fc * m) / (kPi * m);
    };
    const double window = 0.42 -
                          (0.5 * std::cos(2.0 * kPi * n / (kFilterTaps - 1))) +
                          (0.08 * std::cos(4.0 * kPi * n / (kFilterTaps - 1)));
    taps[static_cast<size_t>(n)] = (sinc_lp(hi) - sinc_lp(lo)) * window;
  }
  return taps;
}

// |plane| filtered along each row by |taps|, the row's ends repeated.
std::vector<double> filter_rows(const std::vector<double>& plane, size_t width,
                                size_t height,
                                const std::vector<double>& taps) {
  std::vector<double> out(plane.size(), 0.0);
  const long centre = static_cast<long>(taps.size() / 2);
  const long last = static_cast<long>(width) - 1;
  for (size_t row = 0; row < height; ++row) {
    const double* in = plane.data() + (row * width);
    double* to = out.data() + (row * width);
    for (long x = 0; x <= last; ++x) {
      double acc = 0.0;
      for (size_t k = 0; k < taps.size(); ++k) {
        const long at = std::clamp(x + static_cast<long>(k) - centre, 0L, last);
        acc += taps[k] * in[at];
      }
      to[x] = acc;
    }
  }
  return out;
}

// Each sample's largest |values| within ±|dx| samples and ±|dy| rows.
std::vector<double> neighbourhood_max(const std::vector<double>& values,
                                      size_t width, size_t height, long dx,
                                      long dy) {
  std::vector<double> out(values.size(), 0.0);
  const long w = static_cast<long>(width);
  const long h = static_cast<long>(height);
  for (long row = 0; row < h; ++row) {
    for (long x = 0; x < w; ++x) {
      double peak = 0.0;
      for (long r = std::max(0L, row - dy); r <= std::min(h - 1, row + dy);
           ++r) {
        for (long c = std::max(0L, x - dx); c <= std::min(w - 1, x + dx); ++c) {
          peak = std::max(peak, values[static_cast<size_t>((r * w) + c)]);
        }
      }
      out[static_cast<size_t>((row * w) + x)] = peak;
    }
  }
  return out;
}

// The PAL band edge |hz| moved to the same place relative to |system|'s
// subcarrier.
double band_edge(double hz, orc::VideoSystem system) {
  const double fsc = system == orc::VideoSystem::PAL     ? kPalSubcarrierHz
                     : system == orc::VideoSystem::PAL_M ? 3575611.49
                                                         : 3579545.45;
  return hz * fsc / kPalSubcarrierHz;
}

struct Matrix3 {
  std::array<double, 9> m{};
};

// RGB to XYZ for primaries (x, y) and a D65 white, normalised so white has
// Y = 1 (SMPTE RP 177).
Matrix3 rgb_to_xyz(const std::array<double, 6>& xy) {
  const double wx = 0.3127;
  const double wy = 0.3290;
  std::array<double, 9> p{};
  for (int c = 0; c < 3; ++c) {
    const double x = xy[static_cast<size_t>(2 * c)];
    const double y = xy[static_cast<size_t>((2 * c) + 1)];
    p[static_cast<size_t>(c)] = x / y;
    p[static_cast<size_t>(3 + c)] = 1.0;
    p[static_cast<size_t>(6 + c)] = (1.0 - x - y) / y;
  }
  const std::array<double, 3> w = {wx / wy, 1.0, (1.0 - wx - wy) / wy};
  // Solve P s = w by Cramer's rule.
  auto det = [](const std::array<double, 9>& a) {
    return (a[0] * ((a[4] * a[8]) - (a[5] * a[7]))) -
           (a[1] * ((a[3] * a[8]) - (a[5] * a[6]))) +
           (a[2] * ((a[3] * a[7]) - (a[4] * a[6])));
  };
  const double d = det(p);
  std::array<double, 3> s{};
  for (size_t c = 0; c < 3; ++c) {
    std::array<double, 9> q = p;
    for (size_t r = 0; r < 3; ++r) q[(r * 3) + c] = w[r];
    s[c] = det(q) / d;
  }
  Matrix3 out;
  for (size_t r = 0; r < 3; ++r) {
    for (size_t c = 0; c < 3; ++c) out.m[(r * 3) + c] = p[(r * 3) + c] * s[c];
  }
  return out;
}

const Matrix3& primaries(orc::VideoSystem system) {
  static const Matrix3 kEbu = rgb_to_xyz({0.64, 0.33, 0.29, 0.60, 0.15, 0.06});
  static const Matrix3 kSmpteC =
      rgb_to_xyz({0.630, 0.340, 0.310, 0.595, 0.155, 0.070});
  return system == orc::VideoSystem::PAL ? kEbu : kSmpteC;
}

double lab_f(double t) {
  constexpr double kDelta = 6.0 / 29.0;
  return t > kDelta * kDelta * kDelta
             ? std::cbrt(t)
             : (t / (3.0 * kDelta * kDelta)) + (4.0 / 29.0);
}

double degrees(double radians) { return radians * 180.0 / kPi; }
double radians(double degrees) { return degrees * kPi / 180.0; }

}  // namespace

Region source_region(size_t pixel_x, size_t row, size_t pixel_width,
                     size_t rows, size_t samples_per_line) {
  constexpr size_t kSourceWidth = 720;
  const size_t first = pixel_x * samples_per_line / kSourceWidth;
  const size_t end = (pixel_x + pixel_width) * samples_per_line / kSourceWidth;
  return {first, row, end - first, rows};
}

std::vector<uint8_t> region_mask(size_t width, size_t height,
                                 const Region& region) {
  std::vector<uint8_t> mask(width * height, 0);
  for (size_t row = region.y; row < std::min(height, region.y + region.height);
       ++row) {
    for (size_t x = region.x; x < std::min(width, region.x + region.width);
         ++x) {
      mask[(row * width) + x] = 1;
    }
  }
  return mask;
}

std::vector<uint8_t> mask_and(std::vector<uint8_t> a,
                              const std::vector<uint8_t>& b) {
  for (size_t n = 0; n < a.size() && n < b.size(); ++n) {
    if (b[n] == 0) a[n] = 0;
  }
  return a;
}

Lab yuv_to_lab(double y, double u, double v, orc::VideoSystem system) {
  const Rgb rgb = yuv_to_rgb(y, u, v);
  auto linear = [](double e) { return std::pow(std::clamp(e, 0.0, 1.0), 2.4); };
  const double r = linear(rgb.r);
  const double g = linear(rgb.g);
  const double b = linear(rgb.b);
  // White (R = G = B = 1) is the sum of each row.
  const auto& m = primaries(system).m;
  const double xw = m[0] + m[1] + m[2];
  const double zw = m[6] + m[7] + m[8];
  const double x = (m[0] * r) + (m[1] * g) + (m[2] * b);
  const double yy = (m[3] * r) + (m[4] * g) + (m[5] * b);
  const double z = (m[6] * r) + (m[7] * g) + (m[8] * b);
  const double fx = lab_f(x / xw);
  const double fy = lab_f(yy);
  const double fz = lab_f(z / zw);
  return {(116.0 * fy) - 16.0, 500.0 * (fx - fy), 200.0 * (fy - fz)};
}

double ciede2000(const Lab& first, const Lab& second) {
  const double c1 = std::hypot(first.a, first.b);
  const double c2 = std::hypot(second.a, second.b);
  const double c_mean = (c1 + c2) / 2.0;
  const double c7 = std::pow(c_mean, 7.0);
  const double g = 0.5 * (1.0 - std::sqrt(c7 / (c7 + std::pow(25.0, 7.0))));
  const double a1 = (1.0 + g) * first.a;
  const double a2 = (1.0 + g) * second.a;
  const double cp1 = std::hypot(a1, first.b);
  const double cp2 = std::hypot(a2, second.b);
  auto hue = [](double b, double a) {
    if (a == 0.0 && b == 0.0) return 0.0;
    const double h = degrees(std::atan2(b, a));
    return h < 0.0 ? h + 360.0 : h;
  };
  const double h1 = hue(first.b, a1);
  const double h2 = hue(second.b, a2);

  const double dl = second.l - first.l;
  const double dc = cp2 - cp1;
  double dh = 0.0;
  if (cp1 * cp2 != 0.0) {
    dh = h2 - h1;
    if (dh > 180.0) dh -= 360.0;
    if (dh < -180.0) dh += 360.0;
  }
  const double d_big_h =
      2.0 * std::sqrt(cp1 * cp2) * std::sin(radians(dh / 2.0));

  const double l_mean = (first.l + second.l) / 2.0;
  const double cp_mean = (cp1 + cp2) / 2.0;
  double h_mean = h1 + h2;
  if (cp1 * cp2 != 0.0) {
    if (std::abs(h1 - h2) <= 180.0) {
      h_mean = (h1 + h2) / 2.0;
    } else if (h1 + h2 < 360.0) {
      h_mean = (h1 + h2 + 360.0) / 2.0;
    } else {
      h_mean = (h1 + h2 - 360.0) / 2.0;
    }
  }
  const double t = 1.0 - (0.17 * std::cos(radians(h_mean - 30.0))) +
                   (0.24 * std::cos(radians(2.0 * h_mean))) +
                   (0.32 * std::cos(radians((3.0 * h_mean) + 6.0))) -
                   (0.20 * std::cos(radians((4.0 * h_mean) - 63.0)));
  const double d_theta =
      30.0 * std::exp(-std::pow((h_mean - 275.0) / 25.0, 2.0));
  const double cp7 = std::pow(cp_mean, 7.0);
  const double rc = 2.0 * std::sqrt(cp7 / (cp7 + std::pow(25.0, 7.0)));
  const double l50 = (l_mean - 50.0) * (l_mean - 50.0);
  const double sl = 1.0 + ((0.015 * l50) / std::sqrt(20.0 + l50));
  const double sc = 1.0 + (0.045 * cp_mean);
  const double sh = 1.0 + (0.015 * cp_mean * t);
  const double rt = -std::sin(radians(2.0 * d_theta)) * rc;
  const double tl = dl / sl;
  const double tc = dc / sc;
  const double th = d_big_h / sh;
  return std::sqrt((tl * tl) + (tc * tc) + (th * th) + (rt * tc * th));
}

StillMetrics measure_still(const ComponentPicture& decoded,
                           const ComponentPicture& truth,
                           const std::vector<uint8_t>& mask,
                           const StillMetricOptions& options) {
  StillMetrics result;
  const size_t width = truth.width;
  const size_t height = truth.height;
  if (decoded.width != width || decoded.height != height ||
      mask.size() != width * height || options.sample_rate_hz <= 0.0) {
    return result;
  }
  const double fs = options.sample_rate_hz;

  // Truth chroma magnitude, its edges, and its luma detail near the
  // subcarrier.
  std::vector<double> truth_c(width * height);
  for (size_t n = 0; n < truth_c.size(); ++n) {
    truth_c[n] = std::hypot(truth.u[n], truth.v[n]);
  }
  const std::vector<double> chroma_nearby =
      neighbourhood_max(truth_c, width, height, 2, 1);
  std::vector<double> chroma_step(width * height, 0.0);
  for (size_t row = 0; row < height; ++row) {
    for (size_t x = 0; x < width; ++x) {
      double step = 0.0;
      if (x > 0 && x + 1 < width) {
        const size_t l = truth.index(x - 1, row);
        const size_t r = truth.index(x + 1, row);
        step = std::max(
            step, std::hypot(truth.u[r] - truth.u[l], truth.v[r] - truth.v[l]));
      }
      if (row > 0 && row + 1 < height) {
        const size_t a = truth.index(x, row - 1);
        const size_t b = truth.index(x, row + 1);
        step = std::max(
            step, std::hypot(truth.u[b] - truth.u[a], truth.v[b] - truth.v[a]));
      }
      chroma_step[truth.index(x, row)] = step;
    }
  }
  const std::vector<double> chroma_edge =
      neighbourhood_max(chroma_step, width, height, 1, 1);

  const std::vector<double> luma_high =
      filter_rows(truth.y, width, height,
                  band_filter(band_edge(3.0e6, options.system), fs, fs));
  std::vector<double> luma_high_sq(luma_high.size());
  for (size_t n = 0; n < luma_high.size(); ++n) {
    luma_high_sq[n] = luma_high[n] * luma_high[n];
  }
  const std::vector<double> local_energy = filter_rows(
      luma_high_sq, width, height, std::vector<double>(9, 1.0 / 9.0));

  std::vector<double> y_error(width * height);
  for (size_t n = 0; n < y_error.size(); ++n) {
    y_error[n] = decoded.y[n] - truth.y[n];
  }
  const std::vector<double> y_error_band =
      filter_rows(y_error, width, height,
                  band_filter(band_edge(3.5e6, options.system),
                              band_edge(5.0e6, options.system), fs));

  double ey = 0.0;
  double eu = 0.0;
  double ev = 0.0;
  double e_rgb = 0.0;
  double e_mag = 0.0;
  double e_phase = 0.0;
  double gain_num = 0.0;
  double gain_den = 0.0;
  double e_cross_colour = 0.0;
  double e_cross_luma = 0.0;
  std::vector<double> delta_e;
  delta_e.reserve(width * height);

  for (size_t row = 0; row < height; ++row) {
    for (size_t x = options.edge_exclusion; x + options.edge_exclusion < width;
         ++x) {
      const size_t n = truth.index(x, row);
      if (mask[n] == 0) continue;
      ++result.samples;
      const double dy = decoded.y[n] - truth.y[n];
      const double du = decoded.u[n] - truth.u[n];
      const double dv = decoded.v[n] - truth.v[n];
      ey += dy * dy;
      eu += du * du;
      ev += dv * dv;

      const Rgb a = yuv_to_rgb(decoded.y[n], decoded.u[n], decoded.v[n]);
      const Rgb b = yuv_to_rgb(truth.y[n], truth.u[n], truth.v[n]);
      e_rgb += ((a.r - b.r) * (a.r - b.r)) + ((a.g - b.g) * (a.g - b.g)) +
               ((a.b - b.b) * (a.b - b.b));
      delta_e.push_back(ciede2000(
          yuv_to_lab(truth.y[n], truth.u[n], truth.v[n], options.system),
          yuv_to_lab(decoded.y[n], decoded.u[n], decoded.v[n],
                     options.system)));

      const double decoded_c = std::hypot(decoded.u[n], decoded.v[n]);
      if (truth_c[n] >= kChromaPresent) {
        ++result.chroma_samples;
        const double dm = decoded_c - truth_c[n];
        e_mag += dm * dm;
        gain_num += decoded_c * truth_c[n];
        gain_den += truth_c[n] * truth_c[n];
        double dp = std::atan2(decoded.v[n], decoded.u[n]) -
                    std::atan2(truth.v[n], truth.u[n]);
        dp = std::remainder(dp, 2.0 * kPi);
        e_phase += degrees(dp) * degrees(dp);
      }
      if (chroma_nearby[n] < kChromaAbsent &&
          std::sqrt(local_energy[n]) >= kLumaDetail) {
        ++result.cross_colour_samples;
        e_cross_colour += decoded_c * decoded_c;
      }
      if (chroma_edge[n] >= kChromaEdge) {
        ++result.cross_luma_samples;
        e_cross_luma += y_error_band[n] * y_error_band[n];
      }
    }
  }
  if (result.samples == 0) return result;

  const double count = static_cast<double>(result.samples);
  auto pct = [](double energy, double samples) {
    return samples > 0.0 ? std::sqrt(energy / samples) * 100.0 : 0.0;
  };
  result.y_rms_pct = pct(ey, count);
  result.u_rms_pct = pct(eu, count);
  result.v_rms_pct = pct(ev, count);
  const double mse = e_rgb / (3.0 * count);
  constexpr double kPsnrCeilingDb = 120.0;
  result.rgb_psnr_db = mse > 0.0
                           ? std::min(kPsnrCeilingDb, -10.0 * std::log10(mse))
                           : kPsnrCeilingDb;

  double sum = 0.0;
  for (const double d : delta_e) sum += d;
  result.delta_e_mean = sum / count;
  const size_t p95 =
      std::min(delta_e.size() - 1, static_cast<size_t>(0.95 * count));
  std::nth_element(delta_e.begin(), delta_e.begin() + static_cast<long>(p95),
                   delta_e.end());
  result.delta_e_p95 = delta_e[p95];

  const auto chroma = static_cast<double>(result.chroma_samples);
  result.chroma_magnitude_rms_pct = pct(e_mag, chroma);
  result.chroma_phase_rms_deg =
      chroma > 0.0 ? std::sqrt(e_phase / chroma) : 0.0;
  result.chroma_gain = gain_den > 0.0 ? gain_num / gain_den : 0.0;
  result.cross_colour_rms_pct =
      pct(e_cross_colour, static_cast<double>(result.cross_colour_samples));
  result.cross_luma_rms_pct =
      pct(e_cross_luma, static_cast<double>(result.cross_luma_samples));
  return result;
}

double band_rms_pct(const std::vector<double>& plane, size_t width,
                    size_t height, const std::vector<uint8_t>& mask,
                    size_t edge_exclusion, double low_hz, double high_hz,
                    double sample_rate_hz) {
  if (plane.size() != width * height || mask.size() != plane.size() ||
      sample_rate_hz <= 0.0) {
    return 0.0;
  }
  const std::vector<double> band = filter_rows(
      plane, width, height, band_filter(low_hz, high_hz, sample_rate_hz));
  double energy = 0.0;
  size_t samples = 0;
  for (size_t row = 0; row < height; ++row) {
    for (size_t x = edge_exclusion; x + edge_exclusion < width; ++x) {
      const size_t n = (row * width) + x;
      if (mask[n] == 0) continue;
      energy += band[n] * band[n];
      ++samples;
    }
  }
  return samples == 0
             ? 0.0
             : std::sqrt(energy / static_cast<double>(samples)) * 100.0;
}

std::optional<EdgeResponse> edge_response(const ComponentPicture& picture,
                                          Plane plane, const Region& region,
                                          double sample_rate_hz) {
  if (region.width < 8 || region.height == 0 ||
      region.x + region.width > picture.width ||
      region.y + region.height > picture.height || sample_rate_hz <= 0.0) {
    return std::nullopt;
  }
  const std::vector<double>& values = plane == Plane::Y   ? picture.y
                                      : plane == Plane::U ? picture.u
                                                          : picture.v;
  std::vector<double> profile(region.width, 0.0);
  for (size_t row = region.y; row < region.y + region.height; ++row) {
    for (size_t x = 0; x < region.width; ++x) {
      profile[x] += values[picture.index(region.x + x, row)];
    }
  }
  for (double& p : profile) p /= static_cast<double>(region.height);

  const size_t quarter = region.width / 4;
  double start = 0.0;
  double end = 0.0;
  for (size_t x = 0; x < quarter; ++x) {
    start += profile[x];
    end += profile[region.width - 1 - x];
  }
  start /= static_cast<double>(quarter);
  end /= static_cast<double>(quarter);
  const double step = end - start;
  if (std::abs(step) < 0.05) return std::nullopt;

  std::vector<double> p(profile.size());
  for (size_t x = 0; x < p.size(); ++x) p[x] = (profile[x] - start) / step;

  // The first crossing of |level|, linearly interpolated, at or after
  // |from|.
  auto crossing = [&p](double level, size_t from) -> std::optional<double> {
    for (size_t x = std::max<size_t>(from, 1); x < p.size(); ++x) {
      if (p[x - 1] < level && p[x] >= level) {
        return static_cast<double>(x - 1) +
               ((level - p[x - 1]) / (p[x] - p[x - 1]));
      }
    }
    return std::nullopt;
  };
  const auto t10 = crossing(0.1, 0);
  if (!t10) return std::nullopt;
  const auto t90 = crossing(0.9, static_cast<size_t>(*t10));
  if (!t90) return std::nullopt;

  double over = 0.0;
  for (const double v : p) over = std::max({over, v - 1.0, -v});
  return EdgeResponse{step, (*t90 - *t10) * 1.0e9 / sample_rate_hz,
                      over * 100.0};
}

}  // namespace orc_videosynth
