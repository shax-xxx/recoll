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

#include "debuglog.h"

void Preview::init()
{
    connect(pvTab, SIGNAL(currentChanged(QWidget *)), 
	    this, SLOT(currentChanged(QWidget *)));
    searchTextLine->installEventFilter(this);
    dynSearchActive = false;
    canBeep = true;
}

void Preview::closeEvent(QCloseEvent *e)
{
    emit previewClosed(this);
    QWidget::closeEvent(e);
}

extern int recollNeedsExit;

bool Preview::eventFilter(QObject *target, QEvent *event)
{
    if (event->type() != QEvent::KeyPress) 
	return QWidget::eventFilter(target, event);
    
    LOGDEB1(("Preview::eventFilter: keyEvent\n"));
    QKeyEvent *keyEvent = (QKeyEvent *)event;
    if (keyEvent->key() == Key_Q && (keyEvent->state() & ControlButton)) {
	recollNeedsExit = 1;
	return true;
    } else if (keyEvent->key() ==Key_W &&(keyEvent->state() & ControlButton)) {
	// LOGDEB(("Preview::eventFilter: got ^W\n"));
	closeCurrentTab();
	return true;
    } else if (dynSearchActive) {
	if (keyEvent->key() == Key_F3) {
	    doSearch(true, false);
	    return true;
	}
	if (target != searchTextLine)
	    return QApplication::sendEvent(searchTextLine, event);
    } else {
	QWidget *tw = pvTab->currentPage();
	QWidget *e = 0;
	if (tw)
	    e = (QTextEdit *)tw->child("pvEdit");
	LOGDEB1(("Widget: %p, edit %p, target %p\n", tw, e, target));
	if (e && target == tw && keyEvent->key() == Key_Slash) {
	    searchTextLine->setFocus();
	    dynSearchActive = true;
	    return true;
	}
    }

    return QWidget::eventFilter(target, event);
}

void Preview::searchTextLine_textChanged(const QString & text)
{
    LOGDEB1(("search line text changed. text: '%s'\n", text.ascii()));
    if (text.isEmpty()) {
	dynSearchActive = false;
	nextButton->setEnabled(false);
	prevButton->setEnabled(false);
	clearPB->setEnabled(false);
    } else {
	dynSearchActive = true;
	nextButton->setEnabled(true);
	prevButton->setEnabled(true);
	clearPB->setEnabled(true);
	doSearch(false, false);
    }
}


// Perform text search. If next is true, we look for the next match of the
// current search, trying to advance and possibly wrapping around. If next is
// false, the search string has been modified, we search for the new string, 
// starting from the current position
void Preview::doSearch(bool next, bool reverse)
{
    LOGDEB1(("Preview::doSearch: next %d rev %d\n", int(next), int(reverse)));
    QWidget *tw = pvTab->currentPage();
    QTextEdit *edit = 0;
    if (tw) {
	if ((edit = (QTextEdit*)tw->child("pvEdit")) == 0) {
	    // ??
	    return;
	}
    }
    bool matchCase = matchCheck->isChecked();
    int mspara, msindex, mepara, meindex;
    edit->getSelection(&mspara, &msindex, &mepara, &meindex);
    if (mspara == -1)
	mspara = msindex = mepara = meindex = 0;

    if (next) {
	// We search again, starting from the current match
	if (reverse) {
	    // when searching backwards, have to move back one char
	    if (msindex > 0)
		msindex --;
	    else if (mspara > 0) {
		mspara --;
		msindex = edit->paragraphLength(mspara);
	    }
	} else {
	    // Forward search: start from end of selection
	    mspara = mepara;
	    msindex = meindex;
	    LOGDEB1(("New para: %d index %d\n", mspara, msindex));
	}
    }

    bool found = edit->find(searchTextLine->text(), matchCase, false, 
			      !reverse, &mspara, &msindex);

    if (!found && next && true) { // need a 'canwrap' test here
	if (reverse) {
	    mspara = edit->paragraphs();
	    msindex = edit->paragraphLength(mspara);
	} else {
	    mspara = msindex = 0;
	}
	found = edit->find(searchTextLine->text(), matchCase, false, 
			     !reverse, &mspara, &msindex);
    }

    if (found) {
	canBeep = true;
    } else {
	if (canBeep)
	    QApplication::beep();
	canBeep = false;
    }
}


void Preview::nextPressed()
{
    doSearch(true, false);
}


void Preview::prevPressed()
{
    doSearch(true, true);
}


void Preview::currentChanged(QWidget * tw)
{
    QObject *o = tw->child("pvEdit");
    LOGDEB1(("Preview::currentChanged(). Edit %p\n", o));
    
    if (o == 0) {
	LOGERR(("Editor child not found\n"));
    } else {
	tw->installEventFilter(this);
	o->installEventFilter(this);
	((QWidget*)o)->setFocus();
    }
}


void Preview::closeCurrentTab()
{
    if (pvTab->count() > 1) {
	QWidget *tw = pvTab->currentPage();
	if (tw) 
	    pvTab->removePage(tw);
    } else {
	close();
    }
}


QTextEdit * Preview::addEditorTab()
{
    QWidget *anon = new QWidget((QWidget *)pvTab);
    QVBoxLayout *anonLayout = new QVBoxLayout(anon, 1, 1, "anonLayout"); 
    QTextEdit *editor = new QTextEdit(anon, "pvEdit");
    editor->setReadOnly( TRUE );
    editor->setUndoRedoEnabled( FALSE );
    anonLayout->addWidget(editor);
    pvTab->addTab(anon, "Tab");
    pvTab->showPage(anon);
    return editor;
}

using std::string;

void Preview::setCurTabProps(const Rcl::Doc &doc)
{
    QString title = QString::fromUtf8(doc.title.c_str(), 
				      doc.title.length());
    if (title.length() > 20) {
	title = title.left(10) + "..." + title.right(10);
    }
    pvTab->changeTab(pvTab->currentPage(), title);

    char datebuf[100];
    datebuf[0] = 0;
    if (!doc.fmtime.empty() || !doc.dmtime.empty()) {
	time_t mtime = doc.dmtime.empty() ? 
	    atol(doc.fmtime.c_str()) : atol(doc.dmtime.c_str());
	struct tm *tm = localtime(&mtime);
	strftime(datebuf, 99, "%F %T", tm);
    }
    string tiptxt = doc.url + string("\n");
    tiptxt += doc.mimetype + " " + string(datebuf) + "\n";
    if (!doc.title.empty())
	tiptxt += doc.title + "\n";
    pvTab->setTabToolTip(pvTab->currentPage(),
			 QString::fromUtf8(tiptxt.c_str(), tiptxt.length()));
}
