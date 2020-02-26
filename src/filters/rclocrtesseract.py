#!/usr/bin/env python3
#################################
# Copyright (C) 2020 J.F.Dockes
#   This program is free software; you can redistribute it and/or modify
#   it under the terms of the GNU General Public License as published by
#   the Free Software Foundation; either version 2 of the License, or
#   (at your option) any later version.
#
#   This program is distributed in the hope that it will be useful,
#   but WITHOUT ANY WARRANTY; without even the implied warranty of
#   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
#   GNU General Public License for more details.
#
#   You should have received a copy of the GNU General Public License
#   along with this program; if not, write to the
#   Free Software Foundation, Inc.,
# 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
########################################################

import os
import sys
import atexit
import tempfile
import subprocess
import glob

import rclexecm

_mswindows = (sys.platform == "win32")
if _mswindows:
    ocrlangfile = ".rclocrlang"
else:
    ocrlangfile = ".rclocrlang"

_okexts = ('.tif', '.tiff', '.jpg', '.png', '.jpeg')

def _deb(s):
    if not _mswindows:
        #print("%s" % s, file=sys.stderr)
        pass

def vacuumdir(dir):
    if dir:
        for fn in os.listdir(dir):
            path = os.path.join(dir, fn)
            if os.path.isfile(path):
                os.unlink(path)
    return True

tmpdir = None
def _maybemaketmpdir():
    global tmpdir
    if tmpdir:
        if not vacuumdir(tmpdir):
            _deb("openfile: vacuumdir %s failed" % tmpdir)
            return False
    else:
        tmpdir = tempfile.mkdtemp(prefix='rclmpdf')

def finalcleanup():
    if tmpdir:
        vacuumdir(tmpdir)
        os.rmdir(tmpdir)

atexit.register(finalcleanup)

# Return true if tesseract and the appropriate conversion program for
# the file type (e.g. pdftoppt for pdf) appear to be available
def ocrpossible(path):
    # Check for tesseract
    global tesseract
    tesseract = rclexecm.which("tesseract")
    if not tesseract:
        return False

    # Check input format
    base,ext = os.path.splitext(path)
    ext = ext.lower()
    if ext in _okexts:
        return True

    if ext == '.pdf':
        # Check for pdftoppm. We could use pdftocairo, which can
        # produce a multi-page pdf and make the rest simpler, but the
        # legacy code used pdftoppm for some reason, and it appears
        # that the newest builds from conda-forge do not include
        # pdftocairo. So stay with pdftoppm.
        global pdftoppm
        pdftoppm = rclexecm.which("pdftoppm")
        if pdftoppm:
            return True

    return False


# Try to guess tesseract language. This should depend on the input
# file, but we have no general way to determine it. So use the
# environment and hope for the best.
def _guesstesseractlang(config, path):
    tesseractlang = ""

    dirname = os.path.dirname(path)

    # First look for a language def file in the file's directory
    pdflangfile = os.path.join(dirname, ocrlangfile)
    if os.path.isfile(pdflangfile):
        tesseractlang = open(pdflangfile, "r").read().strip()
    if tesseractlang:
        _deb("Tesseract lang from file: %s" % tesseractlang)
        return tesseractlang

    # Then look for a config file  option.
    config.setKeyDir(dirname)
    tesseractlang = config.getConfParam("tesseractlang")
    if tesseractlang:
        _deb("Tesseract lang from config: %s" % tesseractlang)
        return tesseractlang

    # Half-assed trial to guess from LANG then default to english
    try:
        localelang = os.environ.get("LANG", "").split("_")[0]
        if localelang == "en":
            tesseractlang = "eng"
        elif localelang == "de":
            tesseractlang = "deu"
        elif localelang == "fr":
            tesseractlang = "fra"
    except:
        pass

    if not tesseractlang:
        tesseractlang = "eng"
    _deb("Tesseract lang (guessed): %s" % tesseractlang)
    return tesseractlang

# Process pdf file: use pdftoppm to split it into ppm pages, then run
# tesseract on each and concatenate the result. It would probably be
# possible instead to use pdftocairo to produce a tiff, buf pdftocairo
# is sometimes not available (windows).
def _pdftesseract(config, path):
    if not tmpdir:
        return b""

    tesseractlang = _guesstesseractlang(config, path)

    #tesserrorfile = os.path.join(tmpdir, "tesserrorfile")
    tmpfile = os.path.join(tmpdir, "ocrXXXXXX")

    # Split pdf pages
    try:
        vacuumdir(tmpdir)
        subprocess.check_call([pdftoppm, "-r", "300", path, tmpfile])
    except Exception as e:
        _deb("pdftoppm failed: %s" % e)
        return b""

    files = glob.glob(tmpfile + "*")
    for f in files:
        out = b''
        try:
            out = subprocess.check_output(
                [tesseract, f, f, "-l", tesseractlang],
                stderr=subprocess.STDOUT)
        except Exception as e:
            _deb("tesseract failed: %s" % e)

        errlines = out.split(b'\n')
        if len(errlines) > 2:
            _deb("Tesseract error: %s" % out)

    # Concatenate the result files
    files = glob.glob(tmpfile + "*" + ".txt")
    data = b""
    for f in files:
        data += open(f, "rb").read()

    return data


def _simpletesseract(config, path):
    tesseractlang = _guesstesseractlang(config, path)

    try:
        out = subprocess.check_output(
            [tesseract, path, 'stdout', '-l', tesseractlang],
            stderr=subprocess.DEVNULL)
    except Exception as e:
        _deb("tesseract failed: %s" % e)

    return out


# run ocr on the input path and output the result data.
def runocr(config, path):
    _maybemaketmpdir()
    base,ext = os.path.splitext(path)
    ext = ext.lower()
    if ext in _okexts:
        return _simpletesseract(config, path)
    else:
        return _pdftesseract(config, path)

   


if __name__ == '__main__':
    import rclconfig
    config = rclconfig.RclConfig()
    path =  sys.argv[1]
    if ocrpossible(path):
        data = runocr(config, sys.argv[1])
    else:
        _deb("ocrpossible returned false")
        sys.exit(1)
    sys.stdout.buffer.write(data)
    
