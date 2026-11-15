"""The device editor page is embedded with its Google Fonts import stripped.

Run: python3 -m unittest discover -s tools -p 'test_embed_editor.py'
"""

import gzip
import os
import sys
import unittest

TOOLS = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, TOOLS)

import embed_editor  # noqa: E402

# The exact form the midl editor bundle carries: the CSS is a JS string and the
# URL has semicolons of its own. Stopping at the first `;` left `500;...swap";`
# behind, and the stray quote made the browser drop the whole stylesheet.
BUNDLE_FORM = ('const qw=\'@import"https://fonts.googleapis.com/css2?family=Montserrat:'
               'wght@400;500;600;700&family=JetBrains+Mono:wght@400;500&display=swap";'
               '[data-component=midl-editor]{--bg: #0a121c}\'')


class StripFonts(unittest.TestCase):
    def assertClean(self, src, expected):
        out, n = embed_editor.strip_external_fonts(src)
        self.assertEqual(n, 1)
        self.assertEqual(out, expected)
        self.assertEqual(embed_editor.external_refs(out), [])

    def test_bundle_form_keeps_the_stylesheet_intact(self):
        self.assertClean(BUNDLE_FORM, "const qw='[data-component=midl-editor]{--bg: #0a121c}'")

    def test_url_forms(self):
        self.assertClean("@import url('https://fonts.googleapis.com/css2?family=A:wght@4;7');a{}", "a{}")
        self.assertClean('@import url("//fonts.gstatic.com/x.css") screen;a{}', "a{}")
        self.assertClean("@import url(https://fonts.gstatic.com/x.css);a{}", "a{}")

    def test_a_partial_strip_is_reported(self):
        left = '500;600;700&family=JetBrains+Mono:wght@400;500&display=swap";[a]{}'
        self.assertTrue(embed_editor.external_refs(left))

    def test_the_embedded_page_has_no_font_remnant(self):
        dist = os.path.join(os.path.dirname(TOOLS), "midl", "editor", "device", "dist", "index.html.gz")
        if not os.path.exists(dist):
            self.skipTest("midl submodule not checked out")
        page = gzip.decompress(open(dist, "rb").read()).decode("utf-8")
        out, n = embed_editor.strip_external_fonts(page)
        self.assertGreaterEqual(n, 1)
        self.assertEqual(embed_editor.external_refs(out), [])
        self.assertIn("[data-component=midl-editor]{", out)


if __name__ == "__main__":
    unittest.main()
