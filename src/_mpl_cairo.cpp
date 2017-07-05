#include "_mpl_cairo.h"

namespace mpl_cairo {

using namespace pybind11::literals;

static FT_Library FT_LIB;
static cairo_user_data_key_t const FT_KEY = {0};
static py::object UNIT_CIRCLE;

cairo_matrix_t matrix_from_transform(py::object transform, double y0) {
  if (!py::getattr(transform, "is_affine", py::bool_(true)).cast<bool>()) {
    throw std::invalid_argument("Only affine transforms are handled");
  }
  auto py_matrix = transform.cast<py::array_t<double>>();
  return cairo_matrix_t{
    *py_matrix.data(0, 0), -*py_matrix.data(1, 0),
    *py_matrix.data(0, 1), -*py_matrix.data(1, 1),
    *py_matrix.data(0, 2), y0 - *py_matrix.data(1, 2)};
}

cairo_matrix_t matrix_from_transform(
    py::object transform, cairo_matrix_t* master_matrix) {
  if (!py::getattr(transform, "is_affine", py::bool_(true)).cast<bool>()) {
    throw std::invalid_argument("Only affine transforms are handled");
  }
  auto py_matrix = transform.cast<py::array_t<double>>();
  // The y flip is already handled by the master matrix.
  auto matrix = cairo_matrix_t{
    *py_matrix.data(0, 0), *py_matrix.data(1, 0),
    *py_matrix.data(0, 1), *py_matrix.data(1, 1),
    *py_matrix.data(0, 2), *py_matrix.data(1, 2)};
  cairo_matrix_multiply(&matrix, &matrix, master_matrix);
  return matrix;
}

void copy_for_marker_stamping(cairo_t* orig, cairo_t* dest) {
  cairo_set_antialias(dest, cairo_get_antialias(orig));
  cairo_set_line_cap(dest, cairo_get_line_cap(orig));
  cairo_set_line_join(dest, cairo_get_line_join(orig));
  cairo_set_line_width(dest, cairo_get_line_width(orig));

  auto dash_count = cairo_get_dash_count(orig);
  auto dashes = std::unique_ptr<double[]>(new double[dash_count]);
  double offset;
  cairo_get_dash(orig, dashes.get(), &offset);
  cairo_set_dash(dest, dashes.get(), dash_count, offset);

  double r, g, b, a;
  cairo_pattern_get_rgba(cairo_get_source(orig), &r, &g, &b, &a);
  cairo_set_source_rgba(dest, r, g, b, a);
}

void load_path(cairo_t* cr, py::object path, cairo_matrix_t* matrix) {
  cairo_save(cr);
  cairo_transform(cr, matrix);
  auto vertices = path.attr("vertices").cast<py::array_t<double>>();
  auto maybe_codes = path.attr("codes");
  auto n = vertices.shape(0);
  cairo_new_path(cr);
  if (!maybe_codes.is_none()) {
    auto codes = maybe_codes.cast<py::array_t<int>>();
    for (size_t i = 0; i < n; ++i) {
      auto x0 = *vertices.data(i, 0), y0 = *vertices.data(i, 1);
      auto isfinite = std::isfinite(x0) && std::isfinite(y0);
      switch (static_cast<PathCode>(*codes.data(i))) {
        case PathCode::STOP:
          break;
        case PathCode::MOVETO:
          if (isfinite) {
            cairo_move_to(cr, x0, y0);
          } else {
            cairo_new_sub_path(cr);
          }
          break;
        case PathCode::LINETO:
          if (isfinite) {
            cairo_line_to(cr, x0, y0);
          } else {
            cairo_new_sub_path(cr);
          }
          break;
          // NOTE: The semantics of nonfinite control points seem undocumented.
          // Here, we ignore the entire curve element.
        case PathCode::CURVE3: {
          auto x1 = *vertices.data(i + 1, 0), y1 = *vertices.data(i + 1, 1);
          i += 1;
          isfinite &= std::isfinite(x1) && std::isfinite(y1);
          if (isfinite && cairo_has_current_point(cr)) {
            double x_prev, y_prev;
            cairo_get_current_point(cr, &x_prev, &y_prev);
            cairo_curve_to(cr,
                (x_prev + 2 * x0) / 3, (y_prev + 2 * y0) / 3,
                (2 * x0 + x1) / 3, (2 * y0 + y1) / 3,
                x1, y1);
          } else {
            cairo_new_sub_path(cr);
          }
          break;
        }
        case PathCode::CURVE4: {
          auto x1 = *vertices.data(i + 1, 0), y1 = *vertices.data(i + 1, 1),
               x2 = *vertices.data(i + 2, 0), y2 = *vertices.data(i + 2, 1);
          i += 2;
          isfinite &= std::isfinite(x1) && std::isfinite(y1)
            && std::isfinite(x2) && std::isfinite(y2);
          if (isfinite && cairo_has_current_point(cr)) {
            cairo_curve_to(cr, x0, y0, x1, y1, x2, y2);
          } else {
            cairo_new_sub_path(cr);
          }
          break;
        }
        case PathCode::CLOSEPOLY:
          cairo_close_path(cr);
          break;
      }
    }
  } else {
    for (size_t i = 0; i < n; ++i) {
      auto x = *vertices.data(i, 0), y = *vertices.data(i, 1);
      auto isfinite = std::isfinite(x) && std::isfinite(y);
      if (isfinite) {
        cairo_line_to(cr, x, y);
      } else {
        cairo_new_sub_path(cr);
      }
    }
  }
  cairo_restore(cr);
}

cairo_font_face_t* ft_font_from_prop(py::object prop) {
  // It is probably not worth implementing an additional layer of caching here
  // as findfont already has its cache and object equality needs would also
  // need to go through Python anyways.
  auto font_path =
    py::module::import("matplotlib.font_manager").attr("findfont")(prop)
    .cast<std::string>();
  FT_Face ft_face;
  if (FT_New_Face(FT_LIB, font_path.c_str(), 0, &ft_face)) {
    throw std::runtime_error("FT_New_Face failed");
  }
  auto font_face = cairo_ft_font_face_create_for_ft_face(ft_face, 0);
  if (cairo_font_face_set_user_data(
        font_face, &FT_KEY, ft_face, (cairo_destroy_func_t)FT_Done_Face)) {
    cairo_font_face_destroy(font_face);
    FT_Done_Face(ft_face);
    throw std::runtime_error("cairo_font_face_set_user_data failed");
  }
  return font_face;
}

Region::Region(cairo_rectangle_int_t bbox, std::shared_ptr<char[]> buf) :
  bbox{bbox}, buf{buf} {}

double GraphicsContextRenderer::points_to_pixels(double points) {
  return points * dpi_ / 72;
}

double GraphicsContextRenderer::pixels_to_points(double pixels) {
  return pixels / (dpi_ / 72);
}

rgba_t GraphicsContextRenderer::get_rgba(void) {
  double r, g, b, a;
  auto status = cairo_pattern_get_rgba(cairo_get_source(cr_), &r, &g, &b, &a);
  if (status != CAIRO_STATUS_SUCCESS) {
    throw std::runtime_error(
        "Could not retrieve color from pattern: "
        + std::string{cairo_status_to_string(status)});
  }
  if (alpha_) {
    a = *alpha_;
  }
  return {r, g, b, a};
}

bool GraphicsContextRenderer::try_draw_circles(
    GraphicsContextRenderer& gc,
    py::object marker_path,
    cairo_matrix_t* marker_matrix,
    py::object path,
    cairo_matrix_t* matrix,
    std::optional<py::object> rgb_fc) {
  // Abuse the degenerate-segment handling by cairo to quickly draw fully
  // opaque circles.
  if ((marker_path != UNIT_CIRCLE)
      // yy = -xx due to the flipping in transform_to_matrix().
      || (marker_matrix->yy != -marker_matrix->xx)
      || marker_matrix->xy || marker_matrix->yx
      || !rgb_fc) {
    return false;
  }
  if (rgb_fc) {
    double r, g, b, a{1};
    if (py::len(*rgb_fc) == 3) {
      std::tie(r, g, b) = rgb_fc->cast<rgb_t>();
    } else {
      std::tie(r, g, b, a) = rgb_fc->cast<rgba_t>();
    }
    if (alpha_) {
      a = *alpha_;
    }
    if ((rgba_t{r, g, b, a} != get_rgba())  // Can't draw edge with another color.
        || (a != 1)) {  // Can't handle alpha.
      return false;
    }
  }
  cairo_save(cr_);
  auto [r, g, b, a] = get_rgba();
  cairo_set_source_rgba(cr_, r, g, b, a);
  cairo_transform(cr_, matrix);
  auto vertices = path.attr("vertices").cast<py::array_t<double>>();
  // NOTE: For efficiency, we ignore codes, which is the documented behavior
  // even though not the actual one of other backends.
  auto n = vertices.shape(0);
  for (size_t i = 0; i < n; ++i) {
    auto x = *vertices.data(i, 0), y = *vertices.data(i, 1);
    cairo_move_to(cr_, x, y);
    cairo_close_path(cr_);
  }
  cairo_restore(cr_);
  cairo_save(cr_);
  cairo_set_line_cap(cr_, CAIRO_LINE_CAP_ROUND);
  cairo_set_line_width(cr_, 2 * std::abs(marker_matrix->xx));
  cairo_stroke(cr_);
  cairo_restore(cr_);
  return true;
}

GraphicsContextRenderer::GraphicsContextRenderer(double dpi) :
  cr_{nullptr},
  dpi_{dpi},
  alpha_{{}},
  mathtext_parser_{
    py::module::import("matplotlib.mathtext").attr("MathTextParser")("agg")},
  text2path_{py::module::import("matplotlib.textpath").attr("TextToPath")()} {}

GraphicsContextRenderer::~GraphicsContextRenderer() {
  if (cr_) {
    cairo_destroy(cr_);
  }
}

void GraphicsContextRenderer::set_ctx_from_surface(py::object py_surface) {
  if (cr_) {
    cairo_destroy(cr_);
  }
  if (py_surface.attr("__module__").cast<std::string>() == "cairocffi.surfaces") {
    auto surface = reinterpret_cast<cairo_surface_t*>(
        py::module::import("cairocffi").attr("ffi").attr("cast")(
          "int", py_surface.attr("_pointer")).cast<uintptr_t>());
    cr_ = cairo_create(surface);
  } else {
    throw std::invalid_argument("Could not convert argument to cairo_surface_t*");
  }
}

void GraphicsContextRenderer::set_ctx_from_image_args(
    cairo_format_t format, int width, int height) {
  // NOTE: This API will ultimately be favored over set_ctx_from_surface as
  // it bypasses the need to construct a surface in the Python-level using
  // yet another cairo wrapper.  In particular, cairocffi (which relies on
  // dlopen()) prevents the use of cairo-trace (which relies on LD_PRELOAD).
  if (cr_) {
    cairo_destroy(cr_);
  }
  auto surface = cairo_image_surface_create(format, width, height);
  cr_ = cairo_create(surface);
}

uintptr_t GraphicsContextRenderer::get_data_address() {
  // NOTE: The image buffer is not necessarily contiguous; see
  // cairo_image_surface_get_stride().
  auto surface = cairo_get_target(cr_);
  auto buf = cairo_image_surface_get_data(surface);
  return reinterpret_cast<uintptr_t>(buf);
}

void GraphicsContextRenderer::set_alpha(std::optional<double> alpha) {
  if (!cr_) {
    return;
  }
  alpha_ = alpha;
  if (!alpha) {
    return;
  }
  auto [r, g, b] = get_rgb();
  cairo_set_source_rgba(cr_, r, g, b, *alpha);
}

void GraphicsContextRenderer::set_antialiased(cairo_antialias_t aa) {
  if (!cr_) {
    return;
  }
  cairo_set_antialias(cr_, aa);
}
void GraphicsContextRenderer::set_antialiased(py::object aa) {
  if (!cr_) {
    return;
  }
  cairo_set_antialias(cr_, aa.cast<bool>() ? CAIRO_ANTIALIAS_FAST : CAIRO_ANTIALIAS_NONE);
}

void GraphicsContextRenderer::set_capstyle(std::string capstyle) {
  if (!cr_) {
    return;
  }
  if (capstyle == "butt") {
    cairo_set_line_cap(cr_, CAIRO_LINE_CAP_BUTT);
  } else if (capstyle == "round") {
    cairo_set_line_cap(cr_, CAIRO_LINE_CAP_ROUND);
  } else if (capstyle == "projecting") {
    cairo_set_line_cap(cr_, CAIRO_LINE_CAP_SQUARE);
  } else {
    throw std::invalid_argument("Invalid capstyle: " + capstyle);
  }
}

void GraphicsContextRenderer::set_clip_rectangle(std::optional<py::object> rectangle) {
  if (!cr_ || !rectangle) {
    return;
  }
  // It may be technically necessary to go through one round of restore()
  // / save() (with one save() in the constructor) to clear the previous
  // rectangle (and similarly for set_clip_path()).  In practice, it looks
  // like Matplotlib is careful to only clip once per Python-level context,
  // though, so we're safe.
  auto [x, y, w, h] = rectangle->attr("bounds").cast<rectangle_t>();
  cairo_new_path(cr_);
  cairo_rectangle(cr_, x, get_height() - h - y, w, h);
  cairo_clip(cr_);
}

void GraphicsContextRenderer::set_clip_path(std::optional<py::object> transformed_path) {
  if (!cr_ || !transformed_path) {
    return;
  }
  auto [path, transform] = transformed_path->attr("get_transformed_path_and_affine")()
    .cast<std::tuple<py::object, py::object>>();
  auto matrix = matrix_from_transform(transform, get_height());
  load_path(cr_, path, &matrix);
  cairo_clip(cr_);
}

void GraphicsContextRenderer::set_dashes(
    std::optional<double> dash_offset, std::optional<py::object> dash_list) {
  if (dash_list) {
    if (!dash_offset) {
      throw std::invalid_argument("Missing offset");
    }
    std::vector<double> v;
    for (auto e: dash_list->cast<py::iterable>()) {
      v.push_back(e.cast<double>());
    }
    cairo_set_dash(cr_, v.data(), v.size(), *dash_offset);
  } else {
    cairo_set_dash(cr_, nullptr, 0, 0);
  }
}

void GraphicsContextRenderer::set_foreground(py::object fg, bool /* is_rgba */) {
  auto [r, g, b, a] = py::module::import("matplotlib.colors").attr("to_rgba")(fg)
    .cast<rgba_t>();
  if (alpha_) {
    a = *alpha_;
  }
  cairo_set_source_rgba(cr_, r, g, b, a);
}

void GraphicsContextRenderer::set_joinstyle(std::string joinstyle) {
  if (!cr_) {
    return;
  }
  if (joinstyle == "miter") {
    cairo_set_line_join(cr_, CAIRO_LINE_JOIN_MITER);
  } else if (joinstyle == "round") {
    cairo_set_line_join(cr_, CAIRO_LINE_JOIN_ROUND);
  } else if (joinstyle == "bevel") {
    cairo_set_line_join(cr_, CAIRO_LINE_JOIN_BEVEL);
  } else {
    throw std::invalid_argument("Invalid joinstyle: " + joinstyle);
  }
}

void GraphicsContextRenderer::set_linewidth(double lw) {
  if (!cr_) {
    return;
  }
  cairo_set_line_width(cr_, points_to_pixels(lw));
}

double GraphicsContextRenderer::get_linewidth() {
  return pixels_to_points(cairo_get_line_width(cr_));
}

rgb_t GraphicsContextRenderer::get_rgb(void) {
  auto [r, g, b, a] = get_rgba();
  return {r, g, b};
}

GraphicsContextRenderer& GraphicsContextRenderer::new_gc(void) {
  if (!cr_) {
    return *this;
  }
  cairo_reference(cr_);
  cairo_save(cr_);
  return *this;
}

void GraphicsContextRenderer::copy_properties(GraphicsContextRenderer& other) {
  if (cr_) {
    cairo_destroy(cr_);
  }
  cr_ = other.cr_;
  dpi_ = other.dpi_;
  alpha_ = other.alpha_;
  if (!cr_) {
    return;
  }
  cairo_reference(cr_);
  cairo_save(cr_);
}

void GraphicsContextRenderer::restore(void) {
  if (!cr_) {
    return;
  }
  cairo_restore(cr_);
}

std::tuple<int, int> GraphicsContextRenderer::get_canvas_width_height(void) {
  return {get_width(), get_height()};
}

int GraphicsContextRenderer::get_width(void) {
  if (!cr_) {
    return 0;
  }
  return cairo_image_surface_get_width(cairo_get_target(cr_));
}

int GraphicsContextRenderer::get_height(void) {
  if (!cr_) {
    return 0;
  }
  return cairo_image_surface_get_height(cairo_get_target(cr_));
}

void GraphicsContextRenderer::draw_gouraud_triangles(
    GraphicsContextRenderer& gc,
    py::array_t<double> triangles,
    py::array_t<double> colors,
    py::object transform) {
  if (!cr_) {
    return;
  }
  if (&gc != this) {
    throw std::invalid_argument("Non-matching GraphicsContext");
  }
  auto matrix = matrix_from_transform(transform, get_height());
  cairo_save(cr_);
  auto tri_raw = triangles.unchecked<3>();
  auto col_raw = colors.unchecked<3>();
  auto n = tri_raw.shape(0);
  if ((n != col_raw.shape(0))
      || (tri_raw.shape(1) != 3)
      || (tri_raw.shape(2) != 2)
      || (col_raw.shape(1) != 3)
      || (col_raw.shape(2) != 4)) {
    throw std::invalid_argument("Non-matching shapes");
  }
  auto pattern = cairo_pattern_create_mesh();
  for (size_t i = 0; i < n; ++i) {
    cairo_mesh_pattern_begin_patch(pattern);
    for (size_t j = 0; j < 3; ++j) {
      cairo_mesh_pattern_line_to(
          pattern, *tri_raw.data(i, j, 0), *tri_raw.data(i, j, 1));
      cairo_mesh_pattern_set_corner_color_rgba(
          pattern, j,
          *col_raw.data(i, j, 0), *col_raw.data(i, j, 1),
          *col_raw.data(i, j, 2), *col_raw.data(i, j, 3));
    }
    cairo_mesh_pattern_end_patch(pattern);
  }
  cairo_matrix_invert(&matrix);
  cairo_pattern_set_matrix(pattern, &matrix);
  cairo_set_source(cr_, pattern);
  cairo_paint(cr_);
  cairo_pattern_destroy(pattern);
  cairo_restore(cr_);
}

void GraphicsContextRenderer::draw_image(
    GraphicsContextRenderer& gc, double x, double y, py::array_t<uint8_t> im) {
  if (!cr_) {
    return;
  }
  if (&gc != this) {
    throw std::invalid_argument("Non-matching GraphicsContext");
  }
  cairo_save(cr_);
  auto im_raw = im.unchecked<3>();
  auto ni = im_raw.shape(0), nj = im_raw.shape(1);
  if (im_raw.shape(2) != 4) {
    throw std::invalid_argument("RGBA array must have size (m, n, 4)");
  }
  auto stride = cairo_format_stride_for_width(CAIRO_FORMAT_ARGB32, nj);
  auto buf = std::unique_ptr<uint8_t[]>(new uint8_t[ni * stride]);
  if (alpha_) {
    for (size_t i = 0; i < ni; ++i) {
      auto ptr = reinterpret_cast<uint32_t*>(buf.get() + i * stride);
      for (size_t j = 0; j < nj; ++j) {
        auto r = *im_raw.data(i, j, 0), g = *im_raw.data(i, j, 1),
             b = *im_raw.data(i, j, 2)/*, a = *im_raw.data(i, j, 3) */;
        *(ptr++) =
          (uint8_t(*alpha_ * 0xff) << 24) | (uint8_t(*alpha_ * r) << 16)
          | (uint8_t(*alpha_ * g) << 8) | (uint8_t(*alpha_ * b));
      }
    }
  } else {
    for (size_t i = 0; i < ni; ++i) {
      auto ptr = reinterpret_cast<uint32_t*>(buf.get() + i * stride);
      for (size_t j = 0; j < nj; ++j) {
        auto r = *im_raw.data(i, j, 0), g = *im_raw.data(i, j, 1),
             b = *im_raw.data(i, j, 2), a = *im_raw.data(i, j, 3);
        *(ptr++) =
          (a << 24) | (uint8_t(a / 255. * r) << 16)
          | (uint8_t(a / 255. * g) << 8) | (uint8_t(a / 255. * b));
      }
    }
  }
  auto surface = cairo_image_surface_create_for_data(
      buf.get(), CAIRO_FORMAT_ARGB32, nj, ni, stride);
  auto pattern = cairo_pattern_create_for_surface(surface);
  auto matrix = cairo_matrix_t{1, 0, 0, -1, -x, -y + get_height()};
  cairo_pattern_set_matrix(pattern, &matrix);
  cairo_set_source(cr_, pattern);
  cairo_paint(cr_);
  cairo_pattern_destroy(pattern);
  cairo_surface_destroy(surface);
  cairo_restore(cr_);
}

void GraphicsContextRenderer::draw_markers(
    GraphicsContextRenderer& gc,
    py::object marker_path,
    py::object marker_transform,
    py::object path,
    py::object transform,
    std::optional<py::object> rgb_fc) {
  if (!cr_) {
    return;
  }
  if (&gc != this) {
    throw std::invalid_argument("Non-matching GraphicsContext");
  }

  auto vertices = path.attr("vertices").cast<py::array_t<double>>();
  // NOTE: For efficiency, we ignore codes, which is the documented behavior
  // even though not the actual one of other backends.
  auto n_vertices = vertices.shape(0);

  auto marker_matrix = matrix_from_transform(marker_transform);
  auto matrix = matrix_from_transform(transform, get_height());

  double r, g, b, a{1};
  if (rgb_fc) {
    if (py::len(*rgb_fc) == 3) {
      std::tie(r, g, b) = rgb_fc->cast<rgb_t>();
    } else {
      std::tie(r, g, b, a) = rgb_fc->cast<rgba_t>();
    }
    if (alpha_) {
      a = *alpha_;
    }
  }

  // Recording surfaces are quite slow, so just call a lambda instead.
  auto draw_one_marker = [&](cairo_t* cr) {
    load_path(cr, marker_path, &marker_matrix);
    if (rgb_fc) {
      cairo_save(cr);
      cairo_set_source_rgba(cr, r, g, b, a);
      cairo_fill_preserve(cr);
      cairo_restore(cr);
    }
    cairo_stroke(cr);
  };

  double simplify_threshold = py::module::import("matplotlib").attr("rcParams")[
    "path.simplify_threshold"].cast<double>();
  std::unique_ptr<cairo_pattern_t*[]> patterns;
  size_t n_subpix = 0;
  if (simplify_threshold >= 1. / 16) {
    n_subpix = std::ceil(1 / simplify_threshold);
    if (n_subpix * n_subpix < n_vertices) {
      patterns.reset(new cairo_pattern_t*[n_subpix * n_subpix]);
    }
  }

  if (patterns) {
    // Get the extent of the marker.
    auto recording_surface = cairo_recording_surface_create(
        CAIRO_CONTENT_COLOR_ALPHA, nullptr);
    auto recording_cr = cairo_create(recording_surface);
    copy_for_marker_stamping(cr_, recording_cr);
    draw_one_marker(recording_cr);
    double x0, y0, width, height;
    cairo_recording_surface_ink_extents(recording_surface, &x0, &y0, &width, &height);
    cairo_destroy(recording_cr);
    cairo_surface_destroy(recording_surface);

    for (size_t i = 0; i < n_subpix; ++i) {
      for (size_t j = 0; j < n_subpix; ++j) {
        auto raster_surface = cairo_surface_create_similar_image(
            cairo_get_target(cr_), CAIRO_FORMAT_ARGB32,
            std::ceil(width + 1), std::ceil(height + 1));
        auto raster_cr = cairo_create(raster_surface);
        copy_for_marker_stamping(cr_, raster_cr);
        cairo_translate(
            raster_cr, -x0 + double(i) / n_subpix, -y0 + double(j) / n_subpix);
        draw_one_marker(recording_cr);
        auto pattern = cairo_pattern_create_for_surface(raster_surface);
        cairo_pattern_set_filter(pattern, CAIRO_FILTER_NEAREST);
        patterns[i * n_subpix + j] = pattern;
        cairo_destroy(raster_cr);
        cairo_surface_destroy(raster_surface);
      }
    }

    cairo_save(cr_);
    for (size_t i = 0; i < n_vertices; ++i) {
      auto x = *vertices.data(i, 0), y = *vertices.data(i, 1);
      cairo_matrix_transform_point(&matrix, &x, &y);
      auto target_x = x + x0, target_y = y + y0;
      auto i_target_x = std::floor(target_x), i_target_y = std::floor(target_y);
      auto f_target_x = target_x - i_target_x, f_target_y = target_y - i_target_y;
      auto idx = int(n_subpix * f_target_x) * n_subpix + int(n_subpix * f_target_y);
      auto pattern = patterns[idx];
      // Offsetting by get_height() is already taken care of by matrix.
      auto pattern_matrix = cairo_matrix_t{1, 0, 0, 1, -i_target_x, -i_target_y};
      cairo_pattern_set_matrix(pattern, &pattern_matrix);
      cairo_set_source(cr_, pattern);
      cairo_paint(cr_);
    }
    cairo_restore(cr_);

    // Cleanup.
    for (size_t i = 0; i < n_subpix * n_subpix; ++i) {
      cairo_pattern_destroy(patterns[i]);
    }

  } else {
    if (try_draw_circles(gc, marker_path, &marker_matrix, path, &matrix, rgb_fc)) {
      return;
    }
    for (size_t i = 0; i < n_vertices; ++i) {
      cairo_save(cr_);
      auto x = *vertices.data(i, 0), y = *vertices.data(i, 1);
      cairo_matrix_transform_point(&matrix, &x, &y);
      cairo_translate(cr_, x, y);
      draw_one_marker(cr_);
      cairo_restore(cr_);
    }
  }
}

void GraphicsContextRenderer::draw_path(
    GraphicsContextRenderer& gc,
    py::object path,
    py::object transform,
    std::optional<py::object> rgb_fc) {
  if (!cr_) {
    return;
  }
  if (&gc != this) {
    throw std::invalid_argument("Non-matching GraphicsContext");
  }
  cairo_save(cr_);
  auto matrix = matrix_from_transform(transform, get_height());
  load_path(cr_, path, &matrix);
  if (rgb_fc) {
    cairo_save(cr_);
    double r, g, b, a{1};
    if (py::len(*rgb_fc) == 3) {
      std::tie(r, g, b) = rgb_fc->cast<rgb_t>();
    } else {
      std::tie(r, g, b, a) = rgb_fc->cast<rgba_t>();
    }
    if (alpha_) {
      a = *alpha_;
    }
    cairo_set_source_rgba(cr_, r, g, b, a);
    cairo_fill_preserve(cr_);
    cairo_restore(cr_);
  }
  py::object hatch_path = py::cast(this).attr("get_hatch_path")();
  if (!hatch_path.is_none()) {
    cairo_save(cr_);
    auto dpi = int(dpi_);  // Truncating is good enough.
    auto hatch_surface = cairo_image_surface_create(
        CAIRO_FORMAT_ARGB32, dpi, dpi);
    auto hatch_cr = cairo_create(hatch_surface);
    auto [r, g, b, a] = py::module::import("matplotlib.colors").attr("to_rgba")(
        py::cast(this).attr("get_hatch_color")()).cast<rgba_t>();
    cairo_set_source_rgba(hatch_cr, r, g, b, a);
    cairo_set_line_width(
        hatch_cr,
        points_to_pixels(py::cast(this).attr("get_hatch_linewidth")().cast<double>()));
    auto hatch_matrix = cairo_matrix_t{
      double(dpi), 0, 0, -double(dpi), 0, double(dpi)};
    load_path(hatch_cr, hatch_path, &hatch_matrix);
    cairo_fill_preserve(hatch_cr);
    cairo_stroke(hatch_cr);
    auto hatch_pattern = cairo_pattern_create_for_surface(hatch_surface);
    cairo_pattern_set_extend(hatch_pattern, CAIRO_EXTEND_REPEAT);
    cairo_set_source(cr_, hatch_pattern);
    cairo_clip_preserve(cr_);
    cairo_paint(cr_);
    cairo_pattern_destroy(hatch_pattern);
    cairo_destroy(hatch_cr);
    cairo_surface_destroy(hatch_surface);
    cairo_restore(cr_);
  }
  cairo_stroke(cr_);
  cairo_restore(cr_);
}

void GraphicsContextRenderer::draw_path_collection(
    GraphicsContextRenderer& gc,
    py::object master_transform,
    std::vector<py::object> paths,
    std::vector<py::object> transforms,
    std::vector<py::object> offsets,
    py::object offset_transform,
    std::vector<py::object> fcs,
    std::vector<py::object> ecs,
    std::vector<py::object> lws,
    std::vector<py::object> dashes,
    std::vector<py::object> aas,
    std::vector<py::object> urls,
    std::string offset_position) {
  if (!cr_) {
    return;
  }
  if (&gc != this) {
    throw std::invalid_argument("Non-matching GraphicsContext");
  }
  if (offset_position == "data") {
    // NOTE: This seems to be used only by hexbin().  Perhaps the feature can
    // be deprecated and folded into hexbin()?  For now, we can just fall back
    // to the slow implementation.
    py::module::import("matplotlib.backend_bases")
      .attr("RendererBase").attr("draw_path_collection")(
          this, gc, master_transform,
          paths, transforms, offsets, offset_transform,
          fcs, ecs, lws, dashes, aas, urls, offset_position);
    return;
  }
  auto n_paths = paths.size(),
       n_transforms = transforms.size(),
       n_offsets = offsets.size(),
       n = std::max({n_paths, n_transforms, n_offsets});
  auto master_matrix = matrix_from_transform(master_transform, get_height());
  std::vector<cairo_matrix_t> matrices;
  if (n_transforms) {
    for (size_t i = 0; i < n_transforms; ++i) {
      matrices.push_back(matrix_from_transform(transforms[i], &master_matrix));
    }
  } else {
    n_transforms = 1;
    matrices.push_back(master_matrix);
  }
  auto offset_matrix = matrix_from_transform(offset_transform);
  for (size_t i = 0; i < n; ++i) {
    cairo_save(cr_);
    auto path = paths[i % n_paths];
    auto matrix = matrices[i % n_transforms];
    auto [x, y] = offsets[i % n_offsets].cast<std::pair<double, double>>();
    cairo_matrix_transform_point(&offset_matrix, &x, &y);
    cairo_translate(cr_, x, y);
    load_path(cr_, path, &matrix);
    if (ecs.size()) {
      set_foreground(ecs[i % ecs.size()]);
      cairo_stroke_preserve(cr_);
    }
    if (fcs.size()) {
      auto rgb_fc = fcs[i % fcs.size()];
      double r, g, b, a{1};
      if (py::len(rgb_fc) == 3) {
        std::tie(r, g, b) = rgb_fc.cast<rgb_t>();
      } else {
        std::tie(r, g, b, a) = rgb_fc.cast<rgba_t>();
      }
      if (alpha_) {
        a = *alpha_;
      }
      cairo_set_source_rgba(cr_, r, g, b, a);
      cairo_fill_preserve(cr_);
    }
    // NOTE: We drop antialiaseds because... really?
    // We drop urls as they should be handled in a post-processing step anyways
    // (cairo doesn't seem to support them?).
    cairo_new_path(cr_);
    cairo_restore(cr_);
  }
}

void GraphicsContextRenderer::draw_text(
    GraphicsContextRenderer& gc,
    double x, double y, std::string s, py::object prop, double angle,
    bool ismath, py::object mtext) {
  if (!cr_) {
    return;
  }
  if (&gc != this) {
    throw std::invalid_argument("Non-matching GraphicsContext");
  }
  cairo_save(cr_);
  if (ismath) {
    // FIXME: If angle % 90 == 0, we can round x and y to avoid additional
    // aliasing on top of the one already provided by freetype.  Perhaps
    // we should let it know about the destination subpixel position?  If
    // angle % 90 != 0, all hope is lost anyways.
    if (fmod(angle, 90) == 0) {
      cairo_translate(cr_, round(x), round(y));
    } else {
      cairo_translate(cr_, x, y);
    }
    cairo_rotate(cr_, -angle * M_PI / 180);
    auto [ox, oy, width, height, descent, image, chars] =
      mathtext_parser_.attr("parse")(s, dpi_, prop)
      .cast<std::tuple<
          double, double, double, double, double, py::object, py::object>>();
    auto im_raw = py::array_t<uint8_t, py::array::c_style>{image}.mutable_unchecked<2>();
    auto ni = im_raw.shape(0), nj = im_raw.shape(1);
    auto stride = cairo_format_stride_for_width(CAIRO_FORMAT_A8, nj);
    // 1 byte per pixel!
    auto buf = std::unique_ptr<uint8_t[]>(new uint8_t[ni * stride]);
    for (size_t i = 0; i < ni; ++i) {
      std::memcpy(buf.get() + i * stride, im_raw.data(0, 0) + i * nj, nj);
    }
    auto surface = cairo_image_surface_create_for_data(
        buf.get(), CAIRO_FORMAT_A8, nj, ni, stride);
    cairo_mask_surface(cr_, surface, ox, oy - ni);
    cairo_surface_destroy(surface);
  } else {
    // Need to set the current point (otherwise later texts will just follow,
    // regardless of cairo_translate).
    cairo_translate(cr_, x, y);
    cairo_rotate(cr_, -angle * M_PI / 180);
    cairo_move_to(cr_, 0, 0);
    auto font_face = ft_font_from_prop(prop);
    cairo_set_font_face(cr_, font_face);
    cairo_set_font_size(
        cr_, points_to_pixels(prop.attr("get_size_in_points")().cast<double>()));
    cairo_show_text(cr_, s.c_str());
    cairo_font_face_destroy(font_face);
  }
  cairo_restore(cr_);
}

std::tuple<double, double, double>
GraphicsContextRenderer::get_text_width_height_descent(
    std::string s, py::object prop, bool ismath) {
  if (ismath) {
    auto [ox, oy, width, height, descent, image, chars] =
      mathtext_parser_.attr("parse")(s, dpi_, prop)
      .cast<std::tuple<
          double, double, double, double, double, py::object, py::object>>();
    return {width, height, descent};
  } else {
    cairo_save(cr_);
    auto font_face = ft_font_from_prop(prop);
    cairo_set_font_face(cr_, font_face);
    cairo_set_font_size(
        cr_, points_to_pixels(prop.attr("get_size_in_points")().cast<double>()));
    cairo_text_extents_t extents;
    cairo_text_extents(cr_, s.c_str(), &extents);
    cairo_font_face_destroy(font_face);
    cairo_restore(cr_);
    return {extents.width, extents.height, extents.height + extents.y_bearing};
  }
}

Region GraphicsContextRenderer::copy_from_bbox(py::object bbox) {
  // Use ints to avoid a bunch of warnings below.
  int x0 = std::floor(bbox.attr("x0").cast<double>()),
      x1 = std::ceil(bbox.attr("x1").cast<double>()),
      y0 = std::floor(bbox.attr("y0").cast<double>()),
      y1 = std::ceil(bbox.attr("y1").cast<double>());
  if (!((0 <= x0) && (x0 <= x1) && (x1 < get_width())
        && (0 <= y0) && (y0 <= y1) && (y1 < get_height()))) {
    throw std::invalid_argument("Invalid bbox");
  }
  auto width = x1 - x0, height = y1 - y0;
  // 4 bytes per pixel throughout!
  auto buf = std::shared_ptr<char[]>(new char[4 * width * height]);
  auto surface = cairo_get_target(cr_);
  auto raw = cairo_image_surface_get_data(surface);
  auto stride = cairo_image_surface_get_stride(surface);
  for (int y = y0; y < y1; ++y) {
    std::memcpy(buf.get() + (y - y0) * 4 * width, raw + y * stride + 4 * x0, 4 * width);
  }
  return {{x0, y0, width, height}, buf};
}

void GraphicsContextRenderer::restore_region(Region& region) {
  auto [bbox, buf] = region;
  auto [x0, y0, width, height] = bbox;
  int /* x1 = x0 + width, */ y1 = y0 + height;
  auto surface = cairo_get_target(cr_);
  auto raw = cairo_image_surface_get_data(surface);
  auto stride = cairo_image_surface_get_stride(surface);
  cairo_surface_flush(surface);
  // 4 bytes per pixel!
  for (int y = y0; y < y1; ++y) {
    std::memcpy(raw + y * stride + 4 * x0, buf.get() + (y - y0) * 4 * width, 4 * width);
  }
  cairo_surface_mark_dirty_rectangle(surface, x0, y0, width, height);
}

PYBIND11_PLUGIN(_mpl_cairo) {
  py::module m("_mpl_cairo", "A cairo backend for matplotlib.");

  if (FT_Init_FreeType(&FT_LIB)) {
    throw std::runtime_error("FT_Init_FreeType failed");
  }
  auto clean_ft_lib = py::capsule([]() {
    if (FT_Done_FreeType(FT_LIB)) {
      throw std::runtime_error("FT_Done_FreeType failed");
    }
  });
  m.add_object("_cleanup", clean_ft_lib);
  UNIT_CIRCLE = py::module::import("matplotlib.path").attr("Path").attr("unit_circle")();

  py::enum_<cairo_antialias_t>(m, "antialias_t")
    .value("DEFAULT", CAIRO_ANTIALIAS_DEFAULT)
    .value("NONE", CAIRO_ANTIALIAS_NONE)
    .value("GRAY", CAIRO_ANTIALIAS_GRAY)
    .value("SUBPIXEL", CAIRO_ANTIALIAS_SUBPIXEL)
    .value("FAST", CAIRO_ANTIALIAS_FAST)
    .value("GOOD", CAIRO_ANTIALIAS_GOOD)
    .value("BEST", CAIRO_ANTIALIAS_BEST);
  py::enum_<cairo_format_t>(m, "format_t")
    .value("INVALID", CAIRO_FORMAT_INVALID)
    .value("ARGB32", CAIRO_FORMAT_ARGB32)
    .value("RGB24", CAIRO_FORMAT_RGB24)
    .value("A8", CAIRO_FORMAT_A8)
    .value("A1", CAIRO_FORMAT_A1)
    .value("RGB16_565", CAIRO_FORMAT_RGB16_565)
    .value("RGB30", CAIRO_FORMAT_RGB30);

  py::class_<Region>(m, "_Region");

  py::class_<GraphicsContextRenderer>(m, "GraphicsContextRendererCairo")
    .def(py::init<double>())

    // Backend-specific API.
    .def("set_ctx_from_surface", &GraphicsContextRenderer::set_ctx_from_surface)
    .def("set_ctx_from_image_args", &GraphicsContextRenderer::set_ctx_from_image_args)
    .def("get_data_address", &GraphicsContextRenderer::get_data_address)

    // GraphicsContext API.
    .def("set_alpha", &GraphicsContextRenderer::set_alpha)
    .def("set_antialiased",
        py::overload_cast<cairo_antialias_t>(&GraphicsContextRenderer::set_antialiased))
    .def("set_antialiased",
        py::overload_cast<py::object>(&GraphicsContextRenderer::set_antialiased))
    .def("set_capstyle", &GraphicsContextRenderer::set_capstyle)
    .def("set_clip_rectangle", &GraphicsContextRenderer::set_clip_rectangle)
    .def("set_clip_path", &GraphicsContextRenderer::set_clip_path)
    .def("set_dashes", &GraphicsContextRenderer::set_dashes)
    .def("set_foreground", &GraphicsContextRenderer::set_foreground,
        "fg"_a, "isRGBA"_a=false)
    .def("set_joinstyle", &GraphicsContextRenderer::set_joinstyle)
    .def("set_linewidth", &GraphicsContextRenderer::set_linewidth)

    // Needed by the default impl. of draw_quad_mesh.
    .def("get_linewidth", &GraphicsContextRenderer::get_linewidth)
    // Needed for patheffects.
    .def("get_rgb", &GraphicsContextRenderer::get_rgb)

    .def("new_gc", &GraphicsContextRenderer::new_gc)
    .def("copy_properties", &GraphicsContextRenderer::copy_properties)
    .def("restore", &GraphicsContextRenderer::restore)

    // Renderer API.
    // Needed for patheffects.
    .def_readonly("_text2path", &GraphicsContextRenderer::text2path_)

    .def("get_canvas_width_height", &GraphicsContextRenderer::get_canvas_width_height)
    // Needed for patheffects.
    .def_property_readonly("width", &GraphicsContextRenderer::get_width)
    .def_property_readonly("height", &GraphicsContextRenderer::get_height)

    .def("draw_gouraud_triangles", &GraphicsContextRenderer::draw_gouraud_triangles)
    .def("draw_image", &GraphicsContextRenderer::draw_image)
    .def("draw_markers", &GraphicsContextRenderer::draw_markers,
        "gc"_a, "marker_path"_a, "marker_trans"_a, "path"_a, "trans"_a,
        "rgbFace"_a=nullptr)
    .def("draw_path", &GraphicsContextRenderer::draw_path,
        "gc"_a, "path"_a, "transform"_a, "rgbFace"_a=nullptr)
    .def("draw_path_collection", &GraphicsContextRenderer::draw_path_collection)
    .def("draw_text", &GraphicsContextRenderer::draw_text,
        "gc"_a, "x"_a, "y"_a, "s"_a, "prop"_a, "angle"_a,
        "ismath"_a=false, "mtext"_a=nullptr)
    .def("get_text_width_height_descent",
        &GraphicsContextRenderer::get_text_width_height_descent,
        "s"_a, "prop"_a, "ismath"_a)

    // Canvas API.
    .def("copy_from_bbox", &GraphicsContextRenderer::copy_from_bbox)
    .def("restore_region", &GraphicsContextRenderer::restore_region);

  return m.ptr();
}

}
