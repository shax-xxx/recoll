#!/usr/bin/env python3
# Copyright (C) 2016 J.F.Dockes
# This program is free software; you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation; either version 2 of the License, or
# (at your option) any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the
# Free Software Foundation, Inc.,
# 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.

# Recoll DJVU extractor

import os
import sys
import re
import rclexecm
import subprocess
from rclbasehandler import RclBaseHandler
import rclconfig

ocrproc = None

class DJVUExtractor(RclBaseHandler):

    def __init__(self, em):
        super(DJVUExtractor, self).__init__(em)
        self.config = rclconfig.RclConfig()
        self.confdir = self.config.getConfDir()

        self.djvutxt = rclexecm.which("djvutxt")
        if not self.djvutxt:
            print("RECFILTERROR HELPERNOTFOUND djvutxt")
            sys.exit(1);
        self.djvused = rclexecm.which("djvused")


    def html_text(self, fn):
        self.em.setmimetype('text/html')
        fn = rclexecm.subprocfile(fn)
        # Extract metadata
        metadata = b""
        if self.djvused:
            try:
                metadata = subprocess.check_output(
                    [self.djvused, fn, "-e", "select 1;print-meta"])
            except Exception as e:
                self.em.rclog("djvused failed: %s" % e)
        author = ""
        title = ""
        metadata = metadata.decode('UTF-8', 'replace')
        for line in metadata.split('\n'):
            line = line.split('"')
            if len(line) >= 2:
                nm = line[0].strip()
                if nm == "author":
                    author = ' '.join(line[1:])
                elif nm == "title":
                    title = ' '.join(line[1:])

        # Main text
        txtdata = subprocess.check_output([self.djvutxt, fn])
        if not txtdata:
            self.config.setKeyDir(os.path.dirname(fn))
            s = self.config.getConfParam("djvuocr")
            if rclexecm.configparamtrue(s):
                try:
                    cmd = [sys.executable, os.path.join(_execdir, "rclocr.py"), fn]
                    global ocrproc
                    ocrproc = subprocess.Popen(cmd, stdout=subprocess.PIPE)
                    txtdata, stderr = ocrproc.communicate()
                    ocrproc = None
                except Exception as e:
                    self.em.rclog(f"{cmd} failed: {e}")
                    pass

        txtdata = txtdata.decode('UTF-8', 'replace')

        data = '''<html><head>'''
        data += '''<title>''' + rclexecm.htmlescape(title) + '''</title>'''
        data += '''<meta http-equiv="Content-Type" '''
        data += '''content="text/html;charset=UTF-8">'''
        if author:
            data += '''<meta name="author" content="''' + \
                    rclexecm.htmlescape(author) + '''">'''
        data += '''</head><body><pre>'''

        data += rclexecm.htmlescape(txtdata)
        data += '''</pre></body></html>'''
        return data


# Main program: create protocol handler and extractor and run them
_execdir = os.path.dirname(sys.argv[0])
proto = rclexecm.RclExecM()
extract = DJVUExtractor(proto)
rclexecm.main(proto, extract)
