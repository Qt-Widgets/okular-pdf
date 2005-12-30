// -*- Mode: C++; c-basic-offset: 2; indent-tabs-mode: nil; c-brace-offset: 0; -*-
/**
 * \file dviexport.h
 * Distributed under the GNU GPL version 2 or (at your option)
 * any later version. See accompanying file COPYING or copy at
 * http://www.gnu.org/copyleft/gpl.html
 *
 * \author Angus Leeming
 * \author Stefan Kebekus
 *
 * Classes DVIExportToPDF and DVIExportToPS control the export
 * of a DVI file to PDF or PostScript format, respectively.
 * Common functionality is factored out into a common base class,
 * DVIExport which itself derives from KShared allowing easy,
 * polymorphic storage of multiple KSharedPtr<DVIExport> variables
 * in a container of all exported processes.
 */

#include <config.h>

#include "dviexport.h"

#include "dviFile.h"
#include "dviRenderer.h"
#include "fontprogress.h"
#include "kvs_debug.h"

#include <kfiledialog.h>
#include <klocale.h>
#include <kmessagebox.h>
#include <kprinter.h>
#include <kprocess.h>

#include <QFileInfo>
#include <QLabel>
#include <QTemporaryFile>

#include <cassert>

//#define DVIEXPORT_USE_QPROCESS

DVIExport::DVIExport(dviRenderer& parent, QWidget* parent_widget)
  : started_(false),
    process_(0),
    progress_(0),
    parent_(&parent),
    parent_widget_(parent_widget)
{}


DVIExport::~DVIExport()
{
  delete progress_;
  delete process_;
}


void DVIExport::initialise_progress_dialog(int total_steps,
                                           const QString& label_text,
                                           const QString& whats_this_text,
                                           const QString& tooltip_text,
                                           const QString& caption)
{
  assert(!progress_);
  progress_ = new fontProgressDialog(QString(),
                                     label_text,
                                     QString(),
                                     whats_this_text,
                                     tooltip_text,
                                     parent_widget_,
                                     caption,
                                     false);

  if (progress_) {
    progress_->TextLabel2->setText(i18n("Please be patient"));
    progress_->setTotalSteps(total_steps);
    connect(progress_, SIGNAL(finished()), this, SLOT(abort_process()));
  }
}


void DVIExport::start(const QString& command,
                      const QStringList& args,
                      const QString& working_directory,
                      const QString& error_message)
{
  assert(!process_);

#ifndef DVIEXPORT_USE_QPROCESS
  process_ = new KProcess;
  connect(process_, SIGNAL(receivedStderr(KProcess* , char* , int)), this, SLOT(output_receiver(KProcess* , char* , int)));
  connect(process_, SIGNAL(receivedStdout(KProcess* , char* , int)), this, SLOT(output_receiver(KProcess* , char* , int)));
  connect(process_, SIGNAL(processExited(KProcess* )), this, SLOT(finished(KProcess*)));

  *process_ << command << args;
#else
  process_ = new QProcess;
  process_->setReadChannelMode(QProcess::MergedChannels);
  connect(process_, SIGNAL(readyReadStandardOutput()), this, SLOT(output_receiver()));
  connect(process_, SIGNAL(finished(int)), this, SLOT(finished(int)));
#endif

  if (!working_directory.isEmpty())
    process_->setWorkingDirectory(working_directory);

  error_message_ = error_message;

#ifndef DVIEXPORT_USE_QPROCESS
  if (!process_->start(KProcess::NotifyOnExit, KProcess::AllOutput))
#else
  process_->start(command, args, QIODevice::ReadOnly);
  if (!process_->waitForStarted(-1))
#endif
    kdError(kvs::dvi) << command << " failed to start" << endl;
  else
    started_ = true;
}


void DVIExport::abort_process_impl()
{
  if (progress_) {
    // Explicitly disconnect to prevent a recursive call of abort_process.
    disconnect(progress_, SIGNAL(finished()), 0, 0);
    if (progress_->isVisible())
      progress_->hide();
    delete progress_;
    progress_ = 0;
  }

  // deleting process_ kills the external process itself
  // if it's still running.
  delete process_;
  process_ = 0;
}


#ifndef DVIEXPORT_USE_QPROCESS
void DVIExport::finished_impl()
#else
void DVIExport::finished_impl(int exit_code)
#endif
{
  if (progress_) {
    // Explicitly disconnect to prevent a recursive call of abort_process.
    disconnect(progress_, SIGNAL(finished()), 0, 0);
    if (progress_->isVisible())
      progress_->hide();
  }

#ifndef DVIEXPORT_USE_QPROCESS
  if (process_ && process_->normalExit() && process_->exitStatus() != 0)
#else
  if (process_ && exit_code != 0)
#endif
    KMessageBox::error(parent_widget_, error_message_);
  // Remove this from the store of all export processes.
  parent_->export_finished(this);
}


#ifndef DVIEXPORT_USE_QPROCESS
void DVIExport::output_receiver(KProcess* , char* buffer, int buflen)
#else
void DVIExport::output_receiver()
#endif
{
#ifndef DVIEXPORT_USE_QPROCESS
  // Paranoia.
  if (buflen <= 0)
    return;
#endif

  if (process_) {
#ifndef DVIEXPORT_USE_QPROCESS
    parent_->update_info_dialog(QString::fromLocal8Bit(buffer, buflen));
#else
    parent_->update_info_dialog(process_->readAll());
#endif
    if (progress_)
      progress_->show();
  }
}


namespace {

/** @returns the contents of environment variable @c envname as a
 *  QStringList split at the OS-dependent path separator.
 *  (':' on *nix, ';' on Windows.
 */
const QStringList get_env_path(const char* envname)
{
  if (!envname || !*envname)
    return QStringList();

#if defined _WIN32
  const char path_separator = ';';
#else
  const char path_separator = ':';
#endif

  const char* const envvar = ::getenv(envname);
  if (!envvar || !*envvar)
    return QStringList();

  return QString(envvar).split(path_separator);
}


/** @returns true if the file @c exe_ can be found in the PATH
 *  and is executable.
 */
bool find_exe(const QString& exe_)
{
#if defined _WIN32
  const QFileInfo exe(exe_.endsWith(".exe") ? exe_ : exe_ + ".exe");
#else
  const QFileInfo exe(exe_);
#endif

  if (exe.isAbsolute())
    return exe.exists() && exe.isReadable() && exe.isExecutable();

  const QStringList path = get_env_path("PATH");
  if (path.isEmpty())
    return false;

  typedef QStringList::const_iterator iterator;

  const iterator end = path.end();
  for (iterator it = path.begin(); it != end; ++it) {
    const QString dir = it->endsWith("/") ? *it : *it + '/';
    const QFileInfo abs_exe = dir + exe.filePath();

    if (abs_exe.exists())
      return abs_exe.isReadable() && abs_exe.isExecutable();
  }

  return false;
}

} // namespace anon


DVIExportToPDF::DVIExportToPDF(dviRenderer& parent, QWidget* parent_widget)
  : DVIExport(parent, parent_widget)
{
  // Neither of these should happen. Paranoia checks.
  if (!parent.dviFile)
    return;
  const dvifile& dvi = *(parent.dviFile);

  const QFileInfo input(dvi.filename);
  if (!input.exists() || !input.isReadable())
    return;

  if (!find_exe("dvipdfm")) {
    KMessageBox::sorry(0, i18n("KDVI could not locate the program 'dvipdfm' on your computer. That program is "
                               "essential for the export function to work. You can, however, convert "
                               "the DVI-file to PDF using the print function of KDVI, but that will often "
                               "produce documents which print ok, but are of inferior quality if viewed in the "
                               "Acrobat Reader. It may be wise to upgrade to a more recent version of your "
                               "TeX distribution which includes the 'dvipdfm' program.\n"
                               "Hint to the perplexed system administrator: KDVI uses the PATH environment variable "
                               "when looking for programs."));
    return;
  }

  // Generate a suggestion for a reasonable file name
  const QString suggested_name = dvi.filename.left(dvi.filename.find(".")) + ".pdf";
  const QString output_name = KFileDialog::getSaveFileName(suggested_name, i18n("*.pdf|Portable Document Format (*.pdf)"), parent_widget, i18n("Export File As"));
  if (output_name.isEmpty())
    return;

  const QFileInfo output(output_name);
  if (!output.exists()) {
    const int result =
      KMessageBox::warningContinueCancel(parent_widget,
                                         i18n("The file %1\nexists. Do you want to overwrite that file?").arg(output_name),
                                         i18n("Overwrite File"),
                                         i18n("Overwrite"));
    if (result == KMessageBox::Cancel)
      return;
  }

  initialise_progress_dialog(dvi.total_pages,
                             i18n("Using dvipdfm to export the file to PDF"),
                             i18n("KDVI is currently using the external program 'dvipdfm' to "
                                  "convert your DVI-file to PDF. Sometimes that can take "
                                  "a while because dvipdfm needs to generate its own bitmap fonts "
                                  "Please be patient."),
                             i18n("Waiting for dvipdfm to finish..."),
                             i18n("dvipdfm progress dialog"));

  parent.update_info_dialog(i18n("Export: %1 to PDF").arg(dvi.filename),
                              true);

  start("dvipdfm",
        QStringList() << "-o"
                      << output_name
                      << dvi.filename,
        QFileInfo(dvi.filename).dirPath(true),
        i18n("<qt>The external program 'dvipdfm', which was used to export the file, reported an error. "
             "You might wish to look at the <strong>document info dialog</strong> which you will "
             "find in the File-Menu for a precise error report.</qt>"));
}


DVIExportToPS::DVIExportToPS(dviRenderer& parent,
                             QWidget* parent_widget,
                             const QString& output_name,
                             const QStringList& options,
                             KPrinter* printer)
  : DVIExport(parent, parent_widget),
    printer_(printer)
{
  // None of these should happen. Paranoia checks.
  if (!parent.dviFile)
    return;
  const dvifile& dvi = *(parent.dviFile);

  const QFileInfo input(dvi.filename);
  if (!input.exists() || !input.isReadable())
    return;

  if (dvi.page_offset.isEmpty())
    return;

  if (dvi.numberOfExternalNONPSFiles != 0) {
    KMessageBox::sorry(parent_widget,
                       i18n("<qt><P>This DVI file refers to external graphic files which are not in PostScript format, and cannot be handled by the "
                            "<strong>dvips</strong> program that KDVI uses interally to print or to export to PostScript. The functionality that "
                            "you require is therefore unavailable in this version of KDVI.</p>"
                            "<p>As a workaround, you can use the <strong>File/Export As</strong>-Menu to save this file in PDF format, and then use "
                            "a PDF viewer.</p>"
                            "<p>The author of KDVI apologizes for the inconvenience. If enough users complain, the missing functionality might later "
                            "be added.</p></qt>") ,
                       i18n("Functionality Unavailable"));
    return;
  }

  if (!find_exe("dvips")) {
    KMessageBox::sorry(0, i18n("KDVI could not locate the program 'dvips' on your computer. That program is "
                               "essential for the export function to work.\n"
                               "Hint to the perplexed system administrator: KDVI uses the PATH environment variable "
                               "when looking for programs."));
    return;
  }

  if (!output_name.isEmpty()) {
    output_name_ = output_name;

  } else {
    // Generate a suggestion for a reasonable file name
    const QString suggested_name = dvi.filename.left(dvi.filename.find(".")) + ".ps";
    output_name_ = KFileDialog::getSaveFileName(suggested_name, i18n("*.ps|PostScript (*.ps)"), parent_widget, i18n("Export File As"));
    if (output_name_.isEmpty())
      return;

    const QFileInfo output(output_name_);
    if (!output.exists()) {
      const int result =
        KMessageBox::warningContinueCancel(parent_widget,
                                           i18n("The file %1\nexists. Do you want to overwrite that file?").arg(output_name_),
                                           i18n("Overwrite File"),
                                           i18n("Overwrite"));
      if (result == KMessageBox::Cancel)
        return;
    }
  }

  // There is a major problem with dvips, at least 5.86 and lower: the
  // arguments of the option "-pp" refer to TeX-pages, not to
  // sequentially numbered pages. For instance "-pp 7" may refer to 3
  // or more pages: one page "VII" in the table of contents, a page
  // "7" in the text body, and any number of pages "7" in various
  // appendices, indices, bibliographies, and so forth. KDVI currently
  // uses the following disgusting workaround: if the "options"
  // variable is used, the DVI-file is copied to a temporary file, and
  // all the page numbers are changed into a sequential ordering
  // (using UNIX files, and taking manually care of CPU byte
  // ordering). Finally, dvips is then called with the new file, and
  // the file is afterwards deleted. Isn't that great?

  // A similar problem occurs with DVI files that contain page size
  // information. On these files, dvips pointblank refuses to change
  // the page orientation or set another page size. Thus, if the
  // DVI-file does contain page size information, we remove that
  // information first.

  // input_name is the name of the DVI which is used by dvips, either
  // the original file, or a temporary file with a new numbering.
  QString input_name = dvi.filename;
  if (!options.isEmpty() || dvi.suggestedPageSize != 0) {
    // Get a name for a temporary file.
    // Must open the QTemporaryFile to access the name.
    QTemporaryFile tmpfile;
    tmpfile.open();
    tmpfile_name_ = tmpfile.fileName();
    tmpfile.close();

    input_name = tmpfile_name_;

    fontPool fp;
    dvifile newFile(&dvi, &fp);

    // Renumber pages
    newFile.renumber();

    const quint16 saved_current_page = parent.current_page;
    dvifile* saved_dvi = parent.dviFile;
    parent.dviFile = &newFile;
    parent.errorMsg = QString();

    // Remove any page size information from the file
    for (parent.current_page = 0;
        parent.current_page < newFile.total_pages;
        parent.current_page++)
    {
      if (parent.current_page < newFile.total_pages) {
        parent.command_pointer =
          newFile.dvi_Data() + parent.dviFile->page_offset[parent.current_page];
        parent.end_pointer =
          newFile.dvi_Data() + parent.dviFile->page_offset[parent.current_page+1];
      } else {
        parent.command_pointer = 0;
        parent.end_pointer = 0;
      }

      memset((char*) &parent.currinf.data, 0, sizeof(parent.currinf.data));
      parent.currinf.fonttable = &(parent.dviFile->tn_table);
      parent.currinf._virtual  = 0;
      parent.prescan(&dviRenderer::prescan_removePageSizeInfo);
    }

    parent.current_page = saved_current_page;
    parent.dviFile = saved_dvi;
    newFile.saveAs(input_name);
  }

  initialise_progress_dialog(dvi.total_pages,
                             i18n("Using dvips to export the file to PostScript"),
                             i18n("KDVI is currently using the external program 'dvips' to "
                                  "convert your DVI-file to PostScript. Sometimes that can take "
                                  "a while because dvips needs to generate its own bitmap fonts "
                                  "Please be patient."),
                             i18n("Waiting for dvips to finish..."),
                             i18n("dvips progress dialog"));

  parent.update_info_dialog(i18n("Export: %1 to PostScript").arg(dvi.filename),
                              true);

  QStringList args;
  if (!printer)
    // Export hyperlinks
    args << "-z";

  if (!options.isEmpty())
    args += options;

  args << input_name
       << "-o"
       << output_name_;

  start("dvips",
        args,
        QFileInfo(dvi.filename).dirPath(true),
        i18n("<qt>The external program 'dvips', which was used to export the file, reported an error. "
             "You might wish to look at the <strong>document info dialog</strong> which you will "
             "find in the File-Menu for a precise error report.</qt>"));
}


#ifndef DVIEXPORT_USE_QPROCESS
void DVIExportToPS::finished_impl()
#else
void DVIExportToPS::finished_impl(int exit_code)
#endif
{
  if (printer_ && !output_name_.isEmpty()) {
    const QFileInfo output(output_name_);
    if (output.exists() && output.isReadable())
      printer_->printFiles(QStringList(output_name_), true);
  }

#ifndef DVIEXPORT_USE_QPROCESS
  DVIExport::finished_impl();
#else
  DVIExport::finished_impl(exit_code);
#endif
}


void DVIExportToPS::abort_process_impl()
{
  if (!tmpfile_name_.isEmpty()) {
     // Delete the file.
    QFile(tmpfile_name_).remove();
    tmpfile_name_.clear();
  }

  printer_ = 0;

  DVIExport::abort_process_impl();
}


#include "dviexport.moc"
