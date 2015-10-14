#include "aidl_language.h"

#include <iostream>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>

#include "aidl_language_y.hpp"
#include "logging.h"
#include "parse_helpers.h"

#ifdef _WIN32
int isatty(int  fd)
{
    return (fd == 0);
}
#endif

using android::aidl::IoDelegate;
using android::aidl::cpp_strdup;
using std::cerr;
using std::endl;
using std::string;
using std::unique_ptr;

void yylex_init(void **);
void yylex_destroy(void *);
void yyset_in(FILE *f, void *);
int yyparse(Parser*);
YY_BUFFER_STATE yy_scan_buffer(char *, size_t, void *);
void yy_delete_buffer(YY_BUFFER_STATE, void *);

AidlType::AidlType(const std::string& name, unsigned line,
                   const std::string& comments, bool is_array)
    : name_(name),
      line_(line),
      is_array_(is_array),
      comments_(comments) {}

string AidlType::ToString() const { return name_ + (is_array_ ? "[]" : "");
}

AidlArgument::AidlArgument(AidlArgument::Direction direction, AidlType* type,
                           std::string name, unsigned line)
    : type_(type),
      direction_(direction),
      direction_specified_(true),
      name_(name),
      line_(line) {}

AidlArgument::AidlArgument(AidlType* type, std::string name, unsigned line)
    : type_(type),
      direction_(AidlArgument::IN_DIR),
      direction_specified_(false),
      name_(name),
      line_(line) {}

string AidlArgument::ToString() const {
  string ret;

  if (direction_specified_) {
    switch(direction_) {
    case AidlArgument::IN_DIR:
      ret += "in ";
      break;
    case AidlArgument::OUT_DIR:
      ret += "out ";
      break;
    case AidlArgument::INOUT_DIR:
      ret += "inout ";
      break;
    }
  }

  ret += type_->ToString();
  ret += " ";
  ret += name_;

  return ret;
}

AidlMethod::AidlMethod(bool oneway, AidlType* type, std::string name,
                       std::vector<std::unique_ptr<AidlArgument>>* args,
                       unsigned line, const std::string& comments, int id)
    : oneway_(oneway),
      comments_(comments),
      type_(type),
      name_(name),
      line_(line),
      arguments_(std::move(*args)),
      id_(id) {
  has_id_ = true;
  delete args;
  for (const unique_ptr<AidlArgument>& a : arguments_) {
    if (a->IsIn()) { in_arguments_.push_back(a.get()); }
    if (a->IsOut()) { out_arguments_.push_back(a.get()); }
  }
}

AidlMethod::AidlMethod(bool oneway, AidlType* type, std::string name,
                       std::vector<std::unique_ptr<AidlArgument>>* args,
                       unsigned line, const std::string& comments)
    : AidlMethod(oneway, type, name, args, line, comments, 0) {
  has_id_ = false;
}

Parser::Parser(const IoDelegate& io_delegate)
    : io_delegate_(io_delegate) {
  yylex_init(&scanner_);
}

AidlParcelable::AidlParcelable(AidlQualifiedName* name, unsigned line,
                               const std::string& package)
    : AidlParcelable(name->GetDotName(), line, package) {
  delete name;
}

AidlParcelable::AidlParcelable(const std::string& name, unsigned line,
                               const std::string& package)
    : name_(name),
      line_(line),
      package_(package) {
  item_type = USER_DATA_TYPE;
}

AidlInterface::AidlInterface(const std::string& name, unsigned line,
                             const std::string& comments, bool oneway,
                             std::vector<std::unique_ptr<AidlMethod>>* methods,
                             const std::string& package)
    : name_(name),
      comments_(comments),
      line_(line),
      oneway_(oneway),
      methods_(std::move(*methods)),
      package_(package) {
  item_type = INTERFACE_TYPE_BINDER;
  delete methods;
}

AidlQualifiedName::AidlQualifiedName(std::string term,
                                     std::string comments)
    : terms_({term}),
      comments_(comments) {
}

void AidlQualifiedName::AddTerm(std::string term) {
  terms_.push_back(term);
}

AidlImport::AidlImport(const std::string& from,
                       const std::string& needed_class, unsigned line)
    : from_(from),
      needed_class_(needed_class),
      line_(line) {}

Parser::~Parser() {
  if (raw_buffer_) {
    yy_delete_buffer(buffer_, scanner_);
    raw_buffer_.reset();
  }
  yylex_destroy(scanner_);
}

bool Parser::ParseFile(const string& filename) {
  // Make sure we can read the file first, before trashing previous state.
  unique_ptr<string> new_buffer = io_delegate_.GetFileContents(filename);
  if (!new_buffer) {
    LOG(ERROR) << "Error while opening file for parsing: '" << filename << "'";
    return false;
  }

  // Throw away old parsing state if we have any.
  if (raw_buffer_) {
    yy_delete_buffer(buffer_, scanner_);
    raw_buffer_.reset();
  }

  raw_buffer_ = std::move(new_buffer);
  // We're going to scan this buffer in place, and yacc demands we put two
  // nulls at the end.
  raw_buffer_->append(2u, '\0');
  filename_ = filename;
  package_.clear();
  error_ = 0;
  document_ = nullptr;

  buffer_ = yy_scan_buffer(&(*raw_buffer_)[0], raw_buffer_->length(), scanner_);

  int ret = yy::parser(this).parse();

  return ret == 0 && error_ == 0;
}

void Parser::ReportError(const string& err) {
  /* FIXME: We're printing out the line number as -1. We used to use yylineno
   * (which was NEVER correct even before reentrant parsing). Now we'll need
   * another way.
   */
  cerr << filename_ << ":" << -1 << ": " << err << endl;
  error_ = 1;
}

void Parser::AddImport(AidlQualifiedName* name, unsigned line) {
  imports_.emplace_back(new AidlImport(this->FileName(),
                                       name->GetDotName(), line));
  delete name;
}
