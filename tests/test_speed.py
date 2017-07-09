import pytest

import matplotlib as mpl
from matplotlib.testing.conftest import mpl_test_settings
from matplotlib.figure import Figure
from matplotlib.backends.backend_qt5 import QtGui
import numpy as np

from matplotlib.backends.backend_qt5agg import FigureCanvasQTAgg
from mpl_cairo import antialias_t
from mpl_cairo.qt import FigureCanvasQTCairo


_canvas_classes = [FigureCanvasQTAgg, FigureCanvasQTCairo]
pytest.fixture(autouse=True)(mpl_test_settings)


@pytest.fixture
def axes():
    mpl.rcdefaults()
    fig = Figure()
    return fig.add_subplot(111)


def despine(ax):
    ax.set(xticks=[], yticks=[])
    for spine in ax.spines.values():
        spine.set_visible(False)


@pytest.fixture
def sample_vectors():
    return np.random.RandomState(0).random_sample((2, 10000))


@pytest.fixture
def sample_image():
    return np.random.RandomState(0).random_sample((100, 100))


@pytest.mark.parametrize("canvas_cls", _canvas_classes)
def test_axes(benchmark, canvas_cls, axes):
    axes.figure.canvas = canvas_cls(axes.figure)
    benchmark(axes.figure.canvas.draw)


@pytest.mark.parametrize(
    "canvas_cls,antialiased",
    [(FigureCanvasQTAgg, False),
     (FigureCanvasQTAgg, True),
     (FigureCanvasQTCairo, antialias_t.NONE),
     (FigureCanvasQTCairo, antialias_t.FAST),
     (FigureCanvasQTCairo, antialias_t.GOOD),
     (FigureCanvasQTCairo, antialias_t.BEST)])
@pytest.mark.parametrize("joinstyle", ["miter", "round", "bevel"])
def test_line(
        benchmark, canvas_cls, antialiased, joinstyle, axes, sample_vectors):
    axes.plot(
        *sample_vectors, antialiased=antialiased, solid_joinstyle=joinstyle)
    despine(axes)
    axes.figure.canvas = canvas_cls(axes.figure)
    benchmark(axes.figure.canvas.draw)


@pytest.mark.parametrize("canvas_cls", _canvas_classes)
@pytest.mark.parametrize("threshold", [1 / 8, 0])
@pytest.mark.parametrize("alpha", [.99, 1])
def test_circles(
        benchmark, canvas_cls, threshold, alpha, axes, sample_vectors):
    with mpl.rc_context({"path.simplify_threshold": threshold}):
        axes.plot(*sample_vectors, "o", alpha=alpha)
        despine(axes)
        axes.figure.canvas = canvas_cls(axes.figure)
        benchmark(axes.figure.canvas.draw)


@pytest.mark.parametrize("canvas_cls", _canvas_classes)
@pytest.mark.parametrize("threshold", [1 / 8, 0])
def test_squares(
        benchmark, canvas_cls, threshold, axes, sample_vectors):
    with mpl.rc_context({"path.simplify_threshold": threshold}):
        axes.plot(*sample_vectors, "s")
        despine(axes)
        axes.figure.canvas = canvas_cls(axes.figure)
        benchmark(axes.figure.canvas.draw)


@pytest.mark.parametrize("canvas_cls", _canvas_classes)
@pytest.mark.parametrize("threshold", [1 / 8, 0])
def test_scatter_multicolor(
        benchmark, canvas_cls, threshold, axes, sample_vectors):
    with mpl.rc_context({"path.simplify_threshold": threshold}):
        a, b = sample_vectors
        axes.scatter(a, a, c=b)
        despine(axes)
        axes.figure.canvas = canvas_cls(axes.figure)
        benchmark(axes.figure.canvas.draw)


@pytest.mark.parametrize("canvas_cls", _canvas_classes)
@pytest.mark.parametrize("threshold", [1 / 8, 0])
@pytest.mark.parametrize("marker", ["o", "s"])
def test_scatter_multisize(
        benchmark, canvas_cls, threshold, marker, axes, sample_vectors):
    with mpl.rc_context({"path.simplify_threshold": threshold}):
        a, b = sample_vectors
        axes.scatter(a, a, s=100 * b ** 2, marker=marker)
        despine(axes)
        axes.figure.canvas = canvas_cls(axes.figure)
        benchmark(axes.figure.canvas.draw)


@pytest.mark.parametrize("canvas_cls", _canvas_classes)
def test_image(benchmark, canvas_cls, axes, sample_image):
    axes.imshow(sample_image)
    despine(axes)
    axes.figure.canvas = canvas_cls(axes.figure)
    benchmark(axes.figure.canvas.draw)
