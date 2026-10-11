from __future__ import absolute_import # Use 3.x import model.
import os

# Select the supported binding before any log-viewer module imports pyqtgraph.
os.environ['PYQTGRAPH_QT_LIB'] = 'PyQt6'

#-------------------------------------------------------------------------------
__all__ = ()

from . import plotdockarea
from . import logviewer
