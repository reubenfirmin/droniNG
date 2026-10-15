"""Check log compatibility across the project rename."""

from io import BytesIO
import unittest
from unittest.mock import patch

from droning.telemetry import FileTelemetry, TelemetryBase


class LogHeaderTests(unittest.TestCase):
    def test_new_and_upstream_headers(self):
        for project in (b'droniNG', b'dRonin', b'Tau Labs'):
            with self.subTest(project=project):
                contents = BytesIO(project + b' git hash:\nabc123\nuavohash\n##\n')
                with patch.object(TelemetryBase, '__init__', return_value=None) as init:
                    FileTelemetry(contents, parse_header=True)
                self.assertEqual(init.call_args.kwargs['githash'], 'abc123')
                self.assertEqual(contents.read(), b'##\n')

    def test_missing_header_is_rejected(self):
        with self.assertRaisesRegex(IOError, 'no header signature'):
            FileTelemetry(BytesIO(b'unrecognized log\n'), parse_header=True)


if __name__ == '__main__':
    unittest.main()
