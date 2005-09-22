/****************************************************************************
 ** ui.h extension file, included from the uic-generated form implementation.
 **
 ** If you want to add, delete, or rename functions or slots, use
 ** Qt Designer to update this file, preserving your code.
 **
 ** You should not define a constructor or destructor in this file.
 ** Instead, write your code in functions called init() and destroy().
 ** These will automatically be called by the form's constructor and
 ** destructor.
 *****************************************************************************/

#include <regex.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>

#include <utility>
using std::pair;

#include <qmessagebox.h>
#include <qcstring.h>


#include "rcldb.h"
#include "rclconfig.h"
#include "debuglog.h"
#include "mimehandler.h"
#include "pathut.h"
#include "recoll.h"
#include "internfile.h"
#include "textsplit.h"
#include "smallut.h"
#include "utf8iter.h"
#include "transcode.h"

#include "unacpp.h"
#ifndef MIN
#define MIN(A,B) ((A) < (B) ? (A) : (B))
#endif

void RecollMain::fileExit()
{
    LOGDEB1(("RecollMain: fileExit\n"));
    exit(0);
}

// Misnomer. This is called on a 100ms timer and actually checks for different 
// things apart from a need to exit
void RecollMain::checkExit()
{
    if (indexingstatus) {
	indexingstatus = false;
	// Make sure we reopen the db to get the results.
	LOGINFO(("Indexing done: closing query database\n"));
	rcldb->close();
    }
    if (recollNeedsExit)
	fileExit();
}

void RecollMain::fileStart_IndexingAction_activated()
{
    if (indexingdone == 1)
	startindexing = 1;
}

// Text splitter callback used to take note of the position of query terms 
// inside the result text. This is then used to post highlight tags. 
class myTextSplitCB : public TextSplitCB {
 public:
    const list<string>    *terms;  // in: query terms
    list<pair<int, int> > tboffs;  // out: begin and end positions of
                                   // query terms in text

    myTextSplitCB(const list<string>& terms) 
	: terms(&terms) {
    }

    // Callback called by the text-to-words breaker for each word
    virtual bool takeword(const std::string& term, int pos, int bts, int bte) {
	string dumb;
	Rcl::dumb_string(term, dumb);
	//LOGDEB(("Input dumbbed term: '%s' %d %d %d\n", dumb.c_str(), 
	// pos, bts, bte));
	for (list<string>::const_iterator it = terms->begin(); 
	     it != terms->end(); it++) {
	    if (!stringlowercmp(*it, dumb)) {
		tboffs.push_back(pair<int, int>(bts, bte));
		break;
	    }
	}
	     
	return true;
    }
};

// Fix result text for display inside the gui text window
static string plaintorich(const string &in, const list<string>& terms,
			  list<pair<int, int> >&termoffsets)
{
    LOGDEB(("plaintorich: terms: %s\n", 
	    stringlistdisp(terms).c_str()));

    termoffsets.erase(termoffsets.begin(), termoffsets.end());

    myTextSplitCB cb(terms);
    TextSplit splitter(&cb, true);
    splitter.text_to_words(in);

    for (list<pair<int, int> >::iterator li = cb.tboffs.begin(); 
	 li != cb.tboffs.end(); li++) {
    }

    // State variable used to limitate the number of consecutive empty lines
    int ateol = 0;

    // Rich text output
    string out = "<qt><head><title></title></head><body><p>";

    // Iterator for the list of input term positions. We use it to
    // output highlight tags and to compute term positions in the
    // output text
    list<pair<int, int> >::iterator it = cb.tboffs.begin();

    // Storage for the current term position in output.
    pair<int, int> opos;
    int outbytepos; // This is the current position in output, excluding tags
    for (unsigned int ibyteidx = 0; ibyteidx < in.length(); ibyteidx++) {
	if (it != cb.tboffs.end()) {
	    if (ibyteidx == (unsigned int)it->first) {
		out += "<termtag>";
		opos.first = outbytepos;
	    } else if (ibyteidx == (unsigned int)it->second) {
		if (it != cb.tboffs.end())
		    it++;
		opos.second = outbytepos;
		termoffsets.push_back(opos);
		out += "</termtag>";
	    }
	}
	switch(in[ibyteidx]) {
	case '\n':
	    if (ateol < 2)
		out += "<br>\n";
	    ateol++;
	    outbytepos++;
	    break;
	case '\r': break;
	case '<':
	    ateol = 0;
	    out += "&lt;";
	    outbytepos++;
	    break;
	default:
	    ateol = 0;
	    out += in[ibyteidx];
	    outbytepos++;
	}
    }

    {
	FILE *fp = fopen("/tmp/termsdeb", "w");
	string unaced, ascii;
	fprintf(fp, "plaintorich: text:\n%s\n", out.c_str());
	unac_cpp(out, unaced);
	fprintf(fp, "plaintorich: text:\n%s\n", unaced.c_str());
	transcode(unaced, ascii, "UTF-8", "ASCII");
	fprintf(fp, "plaintorich: text:\n%s\n", ascii.c_str());
	fclose(fp);
    }
    return out;
}

static string urltolocalpath(string url)
{
    return url.substr(7, string::npos);
}

// Use external viewer to display file
void RecollMain::reslistTE_doubleClicked(int par, int)
{
    //    restlistTE_clicked(par, car);
    Rcl::Doc doc;
    int reldocnum =  par - 1;
    if (!rcldb->getDoc(reslist_winfirst + reldocnum, doc, 0))
	return;
    
    // Look for appropriate viewer
    string cmd = getMimeViewer(doc.mimetype, rclconfig->getMimeConf());
    if (cmd.length() == 0) {
	QMessageBox::warning(0, "Recoll", 
			     QString("No external viewer configured for mime"
				     " type ") +
			     doc.mimetype.c_str());
	return;
    }

    string fn = urltolocalpath(doc.url);
    // Substitute %u (url) and %f (file name) inside prototype command
    string ncmd;
    string::const_iterator it1;
    for (it1 = cmd.begin(); it1 != cmd.end();it1++) {
	if (*it1 == '%') {
	    if (++it1 == cmd.end()) {
		ncmd += '%';
		break;
	    }
	    if (*it1 == '%')
		ncmd += '%';
	    if (*it1 == 'u')
		ncmd += "'" + doc.url + "'";
	    if (*it1 == 'f')
		ncmd += "'" + fn + "'";
	} else {
	    ncmd += *it1;
	}
    }

    ncmd += " &";
    LOGDEB(("Executing: '%s'\n", ncmd.c_str()));
    system(ncmd.c_str());
}

// Display preview for the selected document, and highlight entry. The
// paragraph number is doc number in window + 1
void RecollMain::reslistTE_clicked(int par, int car)
{
    LOGDEB(("RecollMain::reslistTE_clicked: par %d, char %d\n", par, car));
    if (reslist_winfirst == -1)
	return;

    Rcl::Doc doc;
    if (reslist_current != -1) {
	QColor color("white");
	reslistTE->setParagraphBackgroundColor(reslist_current+1, color);
    }
    QColor color("lightblue");
    reslistTE->setParagraphBackgroundColor(par, color);

    int reldocnum = par - 1;
    reslist_current = reldocnum;
    previewTextEdit->clear();

    if (!rcldb->getDoc(reslist_winfirst + reldocnum, doc, 0)) {
	QMessageBox::warning(0, "Recoll",
			     QString("Can't retrieve document from database"));
	return;
    }
	
    // Go to the file system to retrieve / convert the document text
    // for preview:
    string fn = urltolocalpath(doc.url);
    Rcl::Doc fdoc;
    FileInterner interner(fn, rclconfig, tmpdir);
    if (interner.internfile(fdoc, doc.ipath) != FileInterner::FIDone) {
	QMessageBox::warning(0, "Recoll",
			     QString("Can't turn doc into internal rep ") +
			     doc.mimetype.c_str());
	return;
    }
    list<string> terms;
    rcldb->getQueryTerms(terms);
    list<pair<int, int> > termoffsets;
    string rich = plaintorich(fdoc.text, terms, termoffsets);

    QStyleSheetItem *item = 
	new QStyleSheetItem( previewTextEdit->styleSheet(), "termtag" );
    item->setColor("blue");
    item->setFontWeight(QFont::Bold);

    QString str = QString::fromUtf8(rich.c_str(), rich.length());
    previewTextEdit->setText(str);
    int para = 0, index = 1;
    if (!termoffsets.empty()) {
	index = (termoffsets.begin())->first;
	LOGDEB(("Preview: Byte index for first term: %d\n", index));
	// Translate byte to character offset
	string::size_type pos = 0;
	Utf8Iter it(rich);
	for (; pos != string::npos && (int)pos < index; it++) {
	    pos = it.getBpos();
	}
	index = pos == string::npos ? 0 : it.getCpos();
	LOGDEB(("Set cursor position: para %d, character index %d\n",
		para,index));
	previewTextEdit->setCursorPosition(0, index);
    }
    previewTextEdit->ensureCursorVisible();
    previewTextEdit->getCursorPosition(&para, &index);
    LOGDEB(("PREVIEW len %d paragraphs: %d. Cpos: %d %d\n", 
	    previewTextEdit->length(), previewTextEdit->paragraphs(), 
	    para, index));
}


// User asked to start query. Run it and call listNextPB_clicked to display
// first page of results
void RecollMain::queryText_returnPressed()
{
    LOGDEB1(("RecollMain::queryText_returnPressed()\n"));
    if (!rcldb->isopen()) {
	string dbdir;
	if (rclconfig->getConfParam(string("dbdir"), dbdir) == 0) {
	    QMessageBox::critical(0, "Recoll",
				  QString("No db directory in configuration"));
	    exit(1);
	}
	dbdir = path_tildexpand(dbdir);
	if (!rcldb->open(dbdir, Rcl::Db::DbRO)) {
	    QMessageBox::information(0, "Recoll",
				     QString("Could not open database in ") + 
				     QString(dbdir) + " wait for indexing " +
				     "to complete?");
	    return;
	}
    }
    if (stemlang.empty()) {
	string param;
	if (rclconfig->getConfParam("querystemming", param))
	    dostem = ConfTree::stringToBool(param);
	else
	    dostem = false;
	if (!rclconfig->getConfParam("querystemminglanguage", stemlang))
	    stemlang = "english";
    }

    reslist_current = -1;
    reslist_winfirst = -1;

    QCString u8 =  queryText->text().utf8();

    if (!rcldb->setQuery(string((const char *)u8), dostem ? 
			 Rcl::Db::QO_STEM : Rcl::Db::QO_NONE, stemlang))
	return;
    list<string> terms;
    listNextPB_clicked();
}


void RecollMain::Search_clicked()
{
    queryText_returnPressed();
}

void RecollMain::clearqPB_clicked()
{
    queryText->clear();
}

static const int respagesize = 10;
void RecollMain::listPrevPB_clicked()
{
    if (reslist_winfirst <= 0)
	return;
    reslist_winfirst -= 2*respagesize;
    listNextPB_clicked();
}


// Fill up result list window with next screen of hits
void RecollMain::listNextPB_clicked()
{
    if (!rcldb)
	return;
    int percent;
    Rcl::Doc doc;
    rcldb->getDoc(0, doc, &percent);
    int resCnt = rcldb->getResCnt();
    LOGDEB(("listNextPB_clicked: rescnt %d, winfirst %d\n", resCnt,
	    reslist_winfirst));

    // If we are already on the last page, nothing to do:
    if (reslist_winfirst >= 0 && (reslist_winfirst + respagesize > resCnt))
	return;

    if (reslist_winfirst < 0)
	reslist_winfirst = 0;
    else
	reslist_winfirst += respagesize;

    bool gotone = false;
    reslistTE->clear();
    previewTextEdit->clear();
    int last = MIN(resCnt-reslist_winfirst, respagesize);

    // Insert results if any in result list window 
    for (int i = 0; i < last; i++) {
	doc.erase();

	if (!rcldb->getDoc(reslist_winfirst + i, doc, &percent)) {
	    if (i == 0) 
		reslist_winfirst = -1;
	    break;
	}
	if (i == 0) {
	    reslistTE->append("<qt><head></head><body><p>");
	    char line[80];
	    sprintf(line, "<p><b>Displaying results %d-%d out of %d</b><br>",
		    reslist_winfirst+1, reslist_winfirst+last, resCnt);
	    reslistTE->append(line);
	}
	    
	gotone = true;

	// Result list display: TOBEDONE
	//  - move abstract/keywords to  Detail window ?
	//  - keywords matched
	//  - language
        //  - size
	char perbuf[10];
	sprintf(perbuf, "%3d%%", percent);
	if (doc.title.empty()) 
	    doc.title = path_getsimple(doc.url);
	char datebuf[100];
	datebuf[0] = 0;
	if (!doc.mtime.empty()) {
	    time_t mtime = atol(doc.mtime.c_str());
	    struct tm *tm = localtime(&mtime);
	    strftime(datebuf, 99, "<i>Modified:</i>&nbsp;%F&nbsp;%T", tm);
	}
	LOGDEB1(("Abstract: %s\n", doc.abstract.c_str()));
	string result = "<p>" + 
	    string(perbuf) + " <b>" + doc.title + "</b><br>" +
	    doc.mimetype + "&nbsp;" +
	    (!doc.mtime.empty() ? string(datebuf) + "<br>" : string("")) +
	    (!doc.abstract.empty() ? doc.abstract + "<br>" : string("")) +
	    (!doc.keywords.empty() ? doc.keywords + "<br>" : string("")) +
	    "<i>" + doc.url + +"</i><br>" +
	    "</p>";

	QString str = QString::fromUtf8(result.c_str(), result.length());
	reslistTE->append(str);
    }

    if (gotone) {
	reslistTE->append("</body></qt>");
	reslistTE->setCursorPosition(0,0);
	reslistTE->ensureCursorVisible();
	// Display preview for 1st doc in list
	reslistTE_clicked(1, 0);
    } else {
	// Restore first in win parameter that we shouln't have incremented
	reslist_winfirst -= respagesize;
	if (reslist_winfirst < 0)
	    reslist_winfirst = -1;
    }
}




void RecollMain::helpQuick_startAction_activated()
{

}


#include "advsearch.h"

advsearch *asearchform;

void RecollMain::advSearchPB_clicked()
{
    if (asearchform == 0) {
	// Couldn't find way to have a normal wm frame
	asearchform = new advsearch(this, "Advanced search", FALSE,
				    WStyle_Customize | WStyle_NormalBorder | 
				    WStyle_Title | WStyle_SysMenu);
	asearchform->setSizeGripEnabled(FALSE);
	asearchform->show();
    } else {
	asearchform->show();
    }
}
