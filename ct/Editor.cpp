//==============================================================================
//
//  Editor.cpp
//
//  Copyright (C) 2013-2020  Greg Utas
//
//  This file is part of the Robust Services Core (RSC).
//
//  RSC is free software: you can redistribute it and/or modify it under the
//  terms of the GNU General Public License as published by the Free Software
//  Foundation, either version 3 of the License, or (at your option) any later
//  version.
//
//  RSC is distributed in the hope that it will be useful, but WITHOUT ANY
//  WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
//  FOR A PARTICULAR PURPOSE.  See the GNU General Public License for more
//  details.
//
//  You should have received a copy of the GNU General Public License along
//  with RSC.  If not, see <http://www.gnu.org/licenses/>.
//
#include "Editor.h"
#include <algorithm>
#include <bitset>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iosfwd>
#include <iterator>
#include <list>
#include <memory>
#include <sstream>
#include "CliThread.h"
#include "CodeCoverage.h"
#include "CodeFile.h"
#include "CxxArea.h"
#include "CxxNamed.h"
#include "CxxScope.h"
#include "CxxScoped.h"
#include "CxxString.h"
#include "CxxSymbols.h"
#include "CxxToken.h"
#include "Debug.h"
#include "Duration.h"
#include "Formatters.h"
#include "Library.h"
#include "NbCliParms.h"
#include "Singleton.h"
#include "SysFile.h"
#include "ThisThread.h"

using std::ostream;
using std::string;
using namespace NodeBase;

//------------------------------------------------------------------------------

namespace CodeTools
{
//  The editors that have modified their original code.  This allows multiple
//  files to be changed (e.g. when a fix requires changes in both a function's
//  declaration and definition).  After all changes needed for a fix have been
//  made, all modified files can be committed.
//
std::set< Editor* > Editors_ = std::set< Editor* >();

//  The number of files committed so far.
//
size_t Commits_ = 0;

//------------------------------------------------------------------------------
//
//  Return codes from editor functions.
//
const word EditAbort = -2;     // stream error or fix not implemented
const word EditFailed = -1;    // could not fix; write info to CLI
const word EditSucceeded = 0;  // warning fixed; write info to CLI
const word EditContinue = 0;   // continue with edit
const word EditCompleted = 1;  // warning closed; don't write info to CLI

//  The last result.  It, and the two functions that manage it, are similar
//  to the SetLastError/GetLastError interface.  Based on the return code,
//  Expl_ should contain
//  o EditFailure or EditAbort: an explanation of why the edit failed
//  o EditSucceeded: an edited line of code (or an empty string after deletion)
//  o EditContinue: an empty string, because there is nothing to report
//  o EditCompleted: an empty string, because all results have been reported
//
string Expl_;

//  Generates a log if Expl_ is not empty.  Then sets Expl_ to EXPL, adding
//  a CRLF if EXPL doesn't have one.
//
void SetExpl(const string& expl);

//  Returns Expl_ after clearing it.
//
string GetExpl();

//------------------------------------------------------------------------------
//
//  Characters that enclose the file name of an #include directive,
//  depending on the group to which it belongs.
//
const string FrontChars = "$%@!<\"";
const string BackChars = "$%@!>\"";

//  Where to add a blank line when inserting new code.
//
enum BlankLocation
{
   BlankNone,
   BlankBefore,
   BlankAfter
};

//------------------------------------------------------------------------------
//
//  Strings for user interaction.
//
fixed_string FixPrompt = "  Fix?";
fixed_string YNSQHelp = "Enter y(yes) n(no) s(skip file) q(quit): ";
fixed_string YNSQChars = "ynsq";
fixed_string FixSkipped = "This fix will be skipped.";
fixed_string NotImplemented = "Fixing this warning is not yet supported.";
fixed_string UnspecifiedFailure = "Internal error. Edit failed.";

fixed_string AccessPrompt = "Enter 0=skip 1=public 2=protected 3=private: ";
fixed_string ArgPrompt = "Choose the argument's name. ";
fixed_string DefnPrompt = "Enter 0=skip 1=default 2=delete: ";
fixed_string FilePrompt = "Enter the filename in which to define ";
fixed_string ShellPrompt = "Do you want to create a shell for the definition?";
fixed_string SuffixPrompt = "Enter a suffix for the name: ";
fixed_string VirtualPrompt = "Enter 0=skip 1=virtual 2=non-virtual: ";

fixed_string ClassInstantiated = "Objects of this class are created, "
                                 "so its constructor must remain public.";

//==============================================================================
//
//  Attributes when declaring a C++ item.
//
struct ItemDeclAttrs
{
   //  Initializes the attributes for an item of type T, with access control A.
   //
   ItemDeclAttrs(Cxx::ItemType t, Cxx::Access a, FunctionRole r = FuncOther);

   //  Initializes the attributes for ITEM.
   //
   explicit ItemDeclAttrs(const CxxToken* item);

   //  Returns the order, within a class, where the item should be declared.
   //
   size_t CalcDeclOrder() const;

   //  The following are provided as inputs.
   //
   const Cxx::ItemType type;  // type of item being declared
   Cxx::Access access;        // desired access control
   FunctionRole role;         // if a function, the type being added
   bool over;                 // set if a function is an override

   //  The following are calculated internally.
   //
   bool isstruct;         // set if item belongs to a struct
   bool oper;             // set for an operator
   bool virt;             // set to make a function virtual
   bool deleted;          // set to define a function as deleted
   bool shell;            // set to create a shell for defining a function
   bool stat;             // set for a static function or data
   bool thisctrl;         // set to insert access control before
   Cxx::Access nextctrl;  // set to insert access control after
   size_t pos;            // position for insertion
   size_t indent;         // number of spaces for indentation
   BlankLocation blank;   // where to insert a blank line
   bool comment;          // set to include a comment
};

//------------------------------------------------------------------------------

ItemDeclAttrs::ItemDeclAttrs(Cxx::ItemType t, Cxx::Access a, FunctionRole r) :
   type(t),
   access(a),
   role(r),
   over(false),
   isstruct(false),
   oper(false),
   virt(false),
   deleted(false),
   shell(false),
   stat(false),
   thisctrl(false),
   nextctrl(Cxx::Access_N),
   pos(string::npos),
   indent(0),
   blank(BlankNone),
   comment(false)
{
   Debug::ft("ItemDeclAttrs.ctor(type)");
}

//------------------------------------------------------------------------------

ItemDeclAttrs::ItemDeclAttrs(const CxxToken* item) :
   type(item->Type()),
   access(item->GetAccess()),
   role(FuncOther),
   over(false),
   isstruct(false),
   oper(false),
   virt(false),
   deleted(false),
   shell(false),
   stat(false),
   thisctrl(false),
   nextctrl(Cxx::Access_N),
   pos(string::npos),
   indent(0),
   blank(BlankNone),
   comment(false)
{
   Debug::ft("ItemDeclAttrs.ctor(item)");

   switch(type)
   {
   case Cxx::Function:
   {
      auto func = static_cast< const Function* >(item);
      role = func->FuncRole();
      over = func->IsOverride();
      oper = (func->FuncType() == FuncOperator);
      virt = func->IsVirtual();
   }
   //  [[fallthrough]]
   case Cxx::Data:
      stat = item->IsStatic();
      break;
   }

   auto cls = item->GetClass();
   if(cls != nullptr) isstruct = (cls->GetClassTag() != Cxx::ClassType);
}

//------------------------------------------------------------------------------

fn_name ItemDeclAttrs_CalcDeclOrder = "ItemDeclAttrs.CalcDeclOrder";

size_t ItemDeclAttrs::CalcDeclOrder() const
{
   Debug::ft(ItemDeclAttrs_CalcDeclOrder);

   //  Items have to be declared in some order, so this tries to organize
   //  them in a consistent way. The first thing that determines order is
   //  the item's access control.
   //
   size_t order = 0;

   switch(access)
   {
   case Cxx::Public:
      order = 0;
      break;
   case Cxx::Protected:
      order = 20;
      break;
   default:
      order = 40;
   }

   switch(type)
   {
   case Cxx::Friend:
      return order + 3;
   case Cxx::Forward:
      return order + 4;
   case Cxx::Enum:
      return order + 5;
   case Cxx::Typedef:
      return order + 6;
   case Cxx::Class:
      return order + 7;

   case Cxx::Function:
   {
      switch(role)
      {
      case PureCtor:
         return order + 8;
      case PureDtor:
         return order + 9;
      case CopyCtor:
         return order + 10;
      case MoveCtor:
         return order + 11;
      case CopyOper:
         return order + 12;
      case MoveOper:
         return order + 13;
      default:
         if(oper) return order + 14;
         if(virt) return order + 16;
         if(over) return order + 17;
         return order + 15;
      }
   }

   case Cxx::Data:
      if(isstruct)
      {
         //  If a struct defines its data first, this will prevent functions
         //  from being added before its data.
         //
         if(stat) return order + 2;
         return order + 1;
      }
      else
      {
         if(stat) return order + 19;
         return order + 18;
      }
   }

   Debug::SwLog(ItemDeclAttrs_CalcDeclOrder, "unexpected item type", type);
   return order;
}

//==============================================================================
//
//  Attributes when inserting a function definition.
//
struct FuncDefnAttrs
{
   explicit FuncDefnAttrs(FunctionRole r);

   const FunctionRole role;  // type of function being inserted
   size_t pos;               // insertion position
   BlankLocation blank;      // where to insert a blank line
   bool rule;                // set to insert a rule
};

FuncDefnAttrs::FuncDefnAttrs(FunctionRole r) :
   role(r),
   pos(string::npos),
   blank(BlankNone),
   rule(false)
{
   Debug::ft("FuncDefnAttrs.ctor");
}

//==============================================================================
//
//  Returns true if ITEM1 and ITEM2 appear in the same statement: specifically,
//  if a semicolon does not appear between their positions.
//
bool AreInSameStatement(const CxxToken* item1, const CxxToken* item2)
{
   Debug::ft("CodeTools.AreInSameStatement");

   if((item1 != nullptr) && (item2 != nullptr))
   {
      auto file1 = item1->GetFile();
      auto file2 = item2->GetFile();
      if(file1 != file2) return false;

      auto pos1 = item1->GetPos();
      auto pos2 = item2->GetPos();
      if(pos1 == pos2) return true;

      auto& editor = file1->GetEditor();
      auto begin = (pos1 < pos2 ? pos1 : pos2);
      auto end = (pos1 > pos2 ? pos1 : pos2);
      return (editor.FindFirstOf(begin, ";") > end);
   }

   return false;
}

//------------------------------------------------------------------------------
//
//  Asks the user to choose declName or defnName as an argument's name.
//
string ChooseArgumentName
   (CliThread& cli, const string& declName, const string& defnName)
{
   Debug::ft("CodeTools.ChooseArgumentName");

   std::ostringstream stream;
   stream << ArgPrompt << CRLF;
   stream << "1=" << QUOTE << declName << QUOTE << SPACE;
   stream << "2=" << QUOTE << defnName << QUOTE << ": ";
   auto choice = cli.IntPrompt(stream.str(), 1, 2);
   return (choice == 1 ? declName : defnName);
}

//------------------------------------------------------------------------------
//
//  Asks the user to determine the attributes for a destructor that is
//  currently non-virtual.
//
word ChooseDtorAttributes(CliThread& cli, ItemDeclAttrs& attrs)
{
   Debug::ft("CodeTools.ChooseDtorAttributes");

   auto access = Cxx::Access_N;
   auto virt = false;
   auto response = cli.IntPrompt(AccessPrompt, 0, 3);

   switch(response)
   {
   case 1:
      access = Cxx::Public;
      break;
   case 2:
      access = Cxx::Protected;
      break;
   case 3:
      access = Cxx::Private;
      break;
   default:
      return EditFailed;
   }

   response = cli.IntPrompt(VirtualPrompt, 0, 2);

   switch(response)
   {
   case 1:
      virt = true;
      break;
   case 2:
      virt = false;
      break;
   default:
      return EditFailed;
   }

   if((attrs.access == access) && (attrs.virt == virt)) return EditCompleted;
   attrs.access = access;
   attrs.virt = virt;
   return EditSucceeded;
}

//------------------------------------------------------------------------------
//
//  Creates a comment for the special member function that will be added
//  according to ATTRS.
//
string CreateSpecialFunctionComment(const ItemDeclAttrs& attrs)
{
   Debug::ft("CodeTools.CreateSpecialFunctionComment");

   string comment;

   if(attrs.deleted)
   {
      //  Currently occurs only for a copy constructor/operator.
      //
      switch(attrs.role)
      {
      case CopyCtor:
      case MoveCtor:
         comment = "Deleted to prohibit copying";
         break;
      case CopyOper:
      case MoveOper:
         comment = "Deleted to prohibit copy assignment";
         break;
      default:
         comment = "[Add comment.]";
      }
   }
   else if(attrs.virt)
   {
      //  For a base class destructor.
      //
      comment = "Virtual to allow subclassing";
   }
   else if(attrs.access == Cxx::Protected)
   {
      //  For a base class (other than the destructor).
      //
      comment = "Protected because this class is virtual";
   }
   else if(attrs.access == Cxx::Private)
   {
      //  For a singleton's constructor or destructor.
      //
      comment = "Private because this is a singleton";
   }
   else
   {
      //  None of the above applies.
      //
      std::ostringstream stream;
      stream << attrs.role;
      comment = stream.str();
      comment.front() = toupper(comment.front());
   }

   comment.push_back('.');
   return comment;
}

//------------------------------------------------------------------------------
//
//  Returns an unquoted string (FLIT) and fn_name identifier (FVAR) that are
//  suitable for invoking Debug::ft.
//
void DebugFtNames(const Function* func, string& flit, string& fvar)
{
   Debug::ft("CodeTools.DebugFtNames");

   //  Get the function's name and the scope in which it appears.
   //
   auto sname = func->GetScope()->Name();
   auto fname = func->DebugName();

   flit = sname;
   if(!sname.empty()) flit.push_back('.');
   flit.append(fname);

   fvar = sname;
   if(!fvar.empty()) fvar.push_back('_');

   if(func->FuncType() == FuncOperator)
   {
      //  Something like "class_operator=" won't pass as an identifier, so use
      //  "class_operatorN", where N is the integer value of the Operator enum.
      //
      auto oper = CxxOp::NameToOperator(fname);
      fname.erase(strlen(OPERATOR_STR));
      fname.append(std::to_string(oper));
   }

   fvar.append(fname);
}

//------------------------------------------------------------------------------
//
//  If CODE is an #include directive, unmangles and returns it, else simply
//  returns it without any changes.
//
string DemangleInclude(string& code)
{
   if(code.find(HASH_INCLUDE_STR) != 0) return code;

   auto front = code.find_first_of(FrontChars);
   if(front == string::npos) return code;
   auto back = rfind_first_not_of(code, code.size() - 1, WhitespaceChars);

   switch(FrontChars.find_first_of(code[front]))
   {
   case 0:
   case 2:
      code[front] = '<';
      code[back] = '>';
      break;
   case 1:
   case 3:
      code[front] = QUOTE;
      code[back] = QUOTE;
      break;
   }

   return code;
}

//------------------------------------------------------------------------------
//
//  Sets FNAME to FLIT, the argument for Debug::ft.  If it is already in use,
//  prompts the user for a suffix to make it unique.  Returns false if FNAME
//  is not unique and the user did not provide a satisfactory suffix, else
//  returns true after enclosing FNAME in quotes.
//
bool EnsureUniqueDebugFtName(CliThread& cli, const string& flit, string& fname)
{
   Debug::ft("CodeTools.EnsureUniqueDebugFtName");

   auto cover = Singleton< CodeCoverage >::Instance();
   fname = flit;

   while(cover->Defined(fname))
   {
      std::ostringstream stream;
      stream << fname << " is already in use. " << SuffixPrompt;
      auto suffix = cli.StrPrompt(stream.str());
      if(suffix.empty()) return false;
      fname = flit + '(' + suffix + ')';
   }

   fname.insert(0, 1, QUOTE);
   fname.push_back(QUOTE);
   return true;
}

//------------------------------------------------------------------------------

CodeFile* FindFuncDefnFile(CliThread& cli, const Class* cls, const string& name)
{
   Debug::ft("CodeTools.FindFuncDefnFile");

   //  Look at all the functions in the class to which the new function
   //  will be added.  If all of them are implemented in the same file,
   //  define the new function in that file, otherwise ask the user to
   //  specify which file should contain the function.
   //
   std::set< CodeFile* > impls;
   auto funcs = cls->Funcs();

   for(auto f = funcs->cbegin(); f != funcs->cend(); ++f)
   {
      auto file = (*f)->GetDefnFile();
      if((file != nullptr) && file->IsCpp()) impls.insert(file);
   }

   CodeFile* file = (impls.size() == 1 ? *impls.cbegin() : nullptr);

   while(file == nullptr)
   {
      std::ostringstream prompt;
      prompt << FilePrompt << CRLF << spaces(2);
      prompt << cls->Name() << SCOPE_STR << name;
      prompt << " ('s' to skip this item): ";
      auto fileName = cli.StrPrompt(prompt.str());
      if(fileName == "s") return nullptr;

      file = Singleton< Library >::Instance()->FindFile(fileName);
      if(file == nullptr)
      {
         cli.Inform("  That file is not in the code library.");
      }
   }

   return file;
}

//------------------------------------------------------------------------------
//
//  Updates BEGIN and END, where ITEM begins and ends.  BEGIN includes any
//  comment that immediately precedes ITEM.  Returns false if ITEM could
//  not be located.
//
bool GetCommentedRange(const CxxToken* item, size_t& begin, size_t& end)
{
   Debug::ft("CodeTools.GetCommentedRange");

   size_t left;
   if(!item->GetRange(begin, left, end)) return false;

   auto& editor = item->GetFile()->GetEditor();
   begin = editor.IntroStart(begin, (item->Type() == Cxx::Function));
   return true;
}

//------------------------------------------------------------------------------

string GetExpl()
{
   Debug::ft("CodeTools.GetExpl");

   auto expl = Expl_;
   Expl_.clear();
   return expl;
}

//------------------------------------------------------------------------------
//
//  Adds a class's items to FUNCS and then sorts them by position.
//
void GetItems(const Class* cls, CxxNamedVector& ivec)
{
   Debug::ft("CodeTools.GetItems");

   auto& items = cls->Items();

   for(auto i = items.cbegin(); i != items.cend(); ++i)
   {
      if((*i)->GetPos() != string::npos)
         ivec.push_back(*i);
   }

   std::sort(ivec.begin(), ivec.end(), IsSortedByPos);
}

//------------------------------------------------------------------------------
//
//  Adds FUNC and its overrides to FUNCS.
//
void GetOverrides(const Function* func, std::vector< const Function* >& funcs)
{
   Debug::ft("CodeTools.GetOverrides");

   funcs.push_back(func);

   auto defn = static_cast< const Function* >(func->GetMate());
   if((defn != nullptr) && (defn != func)) funcs.push_back(defn);

   auto& overs = func->GetOverrides();

   for(auto f = overs.cbegin(); f != overs.cend(); ++f)
   {
      GetOverrides(*f, funcs);
   }
}

//------------------------------------------------------------------------------
//
//  Returns true if the #include in LINE1 should precede that in LINE2.
//
bool IncludesAreSorted(const string& line1, const string& line2)
{
   //  #includes are sorted by group, then alphabetically.  The characters
   //  that enclose the filename distinguish the groups: [] for group 1,
   //  () for group 2, <> for group 3, and "" for group 4.
   //
   auto pos1 = line1.find_first_of(FrontChars);
   auto pos2 = line2.find_first_of(FrontChars);

   if(pos2 == string::npos)
   {
      if(pos1 == string::npos) return (&line1 < &line2);
      return true;
   }
   else if(pos1 == string::npos) return false;

   auto c1 = line1[pos1];
   auto c2 = line2[pos2];
   auto group1 = FrontChars.find(c1);
   auto group2 = FrontChars.find(c2);
   if(group1 < group2) return true;
   if(group1 > group2) return false;
   auto cmp = strCompare(line1, line2);
   if(cmp < 0) return true;
   if(cmp > 0) return false;
   return (&line1 < &line2);
}

//------------------------------------------------------------------------------
//
//  Returns true if a comment precedes ITEM.
//
bool ItemIsCommented(const CxxToken* item)
{
   Debug::ft("CodeTools.ItemIsCommented");

   size_t begin, end;
   GetCommentedRange(item, begin, end);
   auto& editor = item->GetFile()->GetEditor();
   return !editor.OnSameLine(begin, item->GetPos());
}

//------------------------------------------------------------------------------
//
//  Sets Expl_ to "TEXT not found."  If QUOTES is set, TEXT is enclosed in
//  quotes.  Returns 0.
//
word NotFound(fixed_string text, bool quotes = false)
{
   Debug::ft("CodeTools.NotFound");

   string expl;
   if(quotes) expl = QUOTE;
   expl += text;
   if(quotes) expl.push_back(QUOTE);
   expl += " not found.";
   SetExpl(expl);
   return EditFailed;
}

//------------------------------------------------------------------------------
//
//  Sets Expl_ to TEXT and returns RC.
//
word Report(fixed_string text, word rc = EditFailed)
{
   Debug::ft("CodeTools.Report");

   SetExpl(text);
   return rc;
}

//------------------------------------------------------------------------------
//
//  Sets Expl_ to STREAM and returns RC.
//
word Report(const std::ostringstream& stream, word rc = EditFailed)
{
   Debug::ft("CodeTools.Report(stream)");

   SetExpl(stream.str());
   return rc;
}

//------------------------------------------------------------------------------

fn_name CodeTools_SetExpl = "CodeTools.SetExpl";

void SetExpl(const string& expl)
{
   Debug::ft(CodeTools_SetExpl);

   if(!Expl_.empty())
   {
      Debug::SwLog(CodeTools_SetExpl, Expl_, 0);
   }

   Expl_ = expl;
   if(!expl.empty() && expl.back() != CRLF) Expl_.push_back(CRLF);
}

//------------------------------------------------------------------------------
//
//  Returns CODE, indented to LEVEL standard indentations.
//
string strCode(const string& code, size_t level)
{
   return spaces(level * IndentSize()) + code + CRLF;
}

//------------------------------------------------------------------------------
//
//  Returns TEXT, prefixed by "//  " and indented with INDENT leading spaces.
//
string strComment(const string& text, size_t indent)
{
   auto comment = spaces(indent) + "//";
   if(!text.empty()) comment += spaces(2) + text;
   comment.push_back(CRLF);
   return comment;
}

//------------------------------------------------------------------------------
//
//  Invoked when fixing a warning still needs to be implemented.  This
//  normally doesn't occur because WarningAttrs.fixable should be false.
//
word Unimplemented()
{
   Debug::ft("CodeTools.Unimplemented");

   return Report(NotImplemented);
}

//==============================================================================

Editor::Editor() :
   file_(nullptr),
   sorted_(false),
   aliased_(false),
   lastCut_(string::npos)
{
   Debug::ft("Editor.ctor");
}

//------------------------------------------------------------------------------

word Editor::AdjustIndentation(const CodeWarning& log)
{
   Debug::ft("Editor.AdjustIndentation");

   auto begin = CurrBegin(log.Pos());
   if(begin == string::npos) return NotFound("Position for indentation");
   Indent(begin);
   return Changed(begin);
}

//------------------------------------------------------------------------------

word Editor::AdjustOperator(const CodeWarning& log)
{
   Debug::ft("Editor.AdjustOperator");

   auto oper = static_cast< const Operation* >(log.item_);
   auto& attrs = CxxOp::Attrs[oper->Op()];

   if(AdjustSpacing(oper->GetPos(), attrs.symbol.size(), attrs.spacing))
      return Changed(oper->GetPos());
   return NotFound("operator adjustment");
}

//------------------------------------------------------------------------------

word Editor::AdjustPunctuation(const CodeWarning& log)
{
   Debug::ft("Editor.AdjustPunctuation");

   if(log.info_.size() != 2) return NotFound("log information");
   if(AdjustSpacing(log.Pos(), 1, log.info_)) return Changed(log.Pos());
   return NotFound("punctuation adjustment");
}

//------------------------------------------------------------------------------

bool Editor::AdjustSpacing(size_t pos, size_t len, const string& spacing)
{
   Debug::ft("Editor.AdjustSpacing");

   auto changed = false;
   auto prev = pos - 1;
   auto next = pos + len;

   if(spacing[0] == '@')
   {
      auto info = GetLineInfo(pos);
      auto begin = LineRfindNonBlank(prev);
      if(begin < info->depth) begin = info->depth;

      if(begin < prev)
      {
         auto count = prev - begin;
         Erase(begin + 1, count);
         next -= count;
         changed = true;
      }
   }
   else if(spacing[0] == '_')
   {
      if(WhitespaceChars.find(At(prev)) == string::npos)
      {
         Insert(pos, SPACE_STR);
         ++next;
         changed = true;
      }
   }

   if(spacing[1] == '@')
   {
      auto end = LineFindNonBlank(next);
      if(end > next)
      {
         Erase(next, end - next);
         changed = true;
      }
   }
   else if(spacing[1] == '_')
   {
      if(WhitespaceChars.find(At(next)) == string::npos)
      {
         Insert(next, SPACE_STR);
         changed = true;
      }
   }

   return changed;
}

//------------------------------------------------------------------------------

word Editor::AdjustTags(const CodeWarning& log)
{
   Debug::ft("Editor.AdjustTags");

   //  A pointer tag should not be preceded by a space.  It either adheres to
   //  its type or "const".  The same is true for a reference tag, which can
   //  also adhere to a pointer tag.  Even if there is more than one detached
   //  pointer tag, only one log is generated, so fix them all.
   //
   auto tag = (log.warning_ == PtrTagDetached ? '*' : '&');
   auto stop = CurrEnd(log.Pos());
   auto changed = false;

   for(auto pos = source_.find(tag, log.Pos()); pos < stop;
      pos = source_.find(tag, pos + 1))
   {
      if(IsBlank(source_[pos - 1]))
      {
         auto prev = RfindNonBlank(pos - 1);
         auto count = pos - prev - 1;
         Erase(prev + 1, count);
         pos -= count;

         //  If the character after the tag is the beginning of an identifier,
         //  insert a space.
         //
         if(ValidFirstChars.find(source_[pos + 1]) != string::npos)
         {
            Insert(pos + 1, SPACE_STR);
         }

         changed = true;
         break;
      }
   }

   if(changed) return Changed(log.Pos());

   string target = "Detached ";
   target.push_back(tag);
   target.push_back(SPACE);
   return NotFound(target.c_str());
}

//------------------------------------------------------------------------------

word Editor::AdjustVerticalSeparation()
{
   Debug::ft("Editor.AdjustVerticalSeparation");

   auto done = false;

   for(auto n = 1; !done && (n <= 16); ++n)
   {
      auto actions = CheckVerticalSpacing();
      done = true;

      for(size_t pos = 0, i = 0; pos != string::npos; ++i)
      {
         switch(actions[i])
         {
         case LineOK:
            pos = NextBegin(pos);
            break;

         case InsertBlank:
            InsertLine(pos, EMPTY_STR);
            pos = NextBegin(pos);
            pos = NextBegin(pos);
            done = false;
            break;

         case ChangeToEmptyComment:
            Insert(pos, COMMENT_STR);
            pos = NextBegin(pos);
            done = false;
            break;

         case DeleteLine:
            EraseLine(pos);
            done = false;
            break;
         }
      }
   }

   return EditSucceeded;
}

//------------------------------------------------------------------------------

word Editor::ChangeAccess(const CodeWarning& log, Cxx::Access acc)
{
   Debug::ft("Editor.ChangeAccess(log)");

   auto item = const_cast< CxxToken* >(log.item_);
   ItemDeclAttrs attrs(item);
   attrs.access = acc;
   return ChangeAccess(item, attrs);
}

//------------------------------------------------------------------------------

word Editor::ChangeAccess(CxxToken* item, ItemDeclAttrs& attrs)
{
   Debug::ft("Editor.ChangeAccess(item)");

   //  Move the item's declaration and update its access control.  Its
   //  existing comment, if any, will also move, so don't insert one.
   //
   string code;
   auto from = CutCode(item, code);
   auto rc = FindItemDeclLoc(item->GetClass(), item->Name(), attrs);

   if(rc != EditContinue)
   {
      Paste(from, code, from);
      return Report(UnspecifiedFailure, rc);
   }

   attrs.comment = false;
   InsertAfterItemDecl(attrs);
   Paste(attrs.pos, code, from);
   InsertBeforeItemDecl(attrs, EMPTY_STR);
   item->SetAccess(attrs.access);
   return Changed(item->GetPos());
}

//------------------------------------------------------------------------------

word Editor::ChangeClassToNamespace(const CodeWarning& log)
{
   Debug::ft("Editor.ChangeClassToNamespace");

   //  Replace "class" with "namespace" and "static" with "extern" (for data)
   //  or nothing (for functions).  Delete things that are no longer needed:
   //  base class, access controls, special member functions, and closing ';'.
   //
   return Unimplemented();
}

//------------------------------------------------------------------------------

word Editor::ChangeClassToStruct(const CodeWarning& log)
{
   Debug::ft("Editor.ChangeClassToStruct");

   //  Start by changing the class's forward declarations.
   //
   ChangeForwards(log.item_, CLASS_STR, STRUCT_STR);

   //  Look for the class's name and then back up to "class".
   //
   auto pos = log.item_->GetPos();
   if(pos == string::npos) return NotFound("Class name");
   pos = Rfind(pos, CLASS_STR);
   if(pos == string::npos) return NotFound(CLASS_STR, true);
   Replace(pos, strlen(CLASS_STR), STRUCT_STR);

   //  If the class began with a "public:" access control, erase it.
   //
   auto left = Find(pos, "{");
   if(left == string::npos) return NotFound("Left brace");
   auto access = FindWord(left + 1, PUBLIC_STR);
   if(access != string::npos)
   {
      auto colon = FindNonBlank(access + strlen(PUBLIC_STR));
      Erase(colon, 1);
      Erase(access, strlen(PUBLIC_STR));
      if(IsBlankLine(access)) EraseLine(access);
   }
   return Changed(pos);
}

//------------------------------------------------------------------------------

word Editor::Changed()
{
   Debug::ft("Editor.Changed");

   Editors_.insert(this);
   return EditSucceeded;
}

//------------------------------------------------------------------------------

word Editor::Changed(size_t pos)
{
   Debug::ft("Editor.Changed(pos)");

   auto code = GetCode(pos);
   SetExpl(IsBlankLine(pos) ? EMPTY_STR : code);
   Editors_.insert(this);
   return EditSucceeded;
}

//------------------------------------------------------------------------------

word Editor::ChangeDataToFree(const CodeWarning& log)
{
   Debug::ft("Editor.ChangeDataToFree");

   auto decl = static_cast< const Data* >(log.item_);
   auto defn = decl->GetMate();
   if(defn == nullptr) return NotFound("Data definition");
   auto cls = decl->GetClass();
   auto qual = cls->Name() + SCOPE_STR;
   auto cpp = defn->GetFile();
   auto& refs = decl->Xref();
   auto ext = false;

   //  Remove the class name qualifier on references to the data and note
   //  whether any references occur outside the file that defines the data.
   //
   for(auto r = refs.cbegin(); r != refs.cend(); ++r)
   {
      if(AreInSameStatement(decl, *r)) continue;
      if(AreInSameStatement(defn, *r)) continue;

      auto file = (*r)->GetFile();
      if(file != cpp) ext = true;

      auto& editor = file->GetEditor();
      auto pos = editor.LineRfind((*r)->GetPos(), qual);
      if(pos != string::npos)
      {
         editor.Erase(pos, qual.size());
      }
   }

   //  Find the extent of the data's definition.  The definition will move
   //  to file scope, so remove any indentation.  Cut the definition after
   //  erasing its "static" tag and prefixing an "extern" tag.
   //
   size_t begin, end;
   if(!GetCommentedRange(decl, begin, end))
      return NotFound("Data definition");

   for(auto pos = CurrBegin(end); pos >= begin; pos = PrevBegin(pos))
   {
      auto blanks = LineFindFirst(pos) - pos;
      Erase(pos, blanks);
   }

   begin = CurrBegin(decl->GetPos());
   auto pos = LineFind(begin, STATIC_STR);
   if(pos == string::npos) return NotFound(STATIC_STR, true);
   Erase(pos, strlen(STATIC_STR) + 1);
   auto code = string(EXTERN_STR) + SPACE;
   Insert(begin, code);

   code.clear();
   if(CutCode(decl, code) == string::npos) return EditFailed;

   if(ext)
   {
      //  The data was referenced outside of the .cpp that defines it, so
      //  move its declaration directly before the class that declared it.
      //  This code is currently unreachable, because DataCouldBeFree is
      //  only logged on a data that is not used outside the .cpp.
      //
      if(!GetCommentedRange(cls, begin, end))
         return NotFound("Data's class");
      pos = CurrBegin(begin);
      InsertLine(pos, EMPTY_STR);
      InsertLine(pos, code);
   }
   else
   {
      //  The data was only referenced within the .cpp that defines it, so
      //  discard its declaration while saving any comment that preceded it.
      //
      code.erase(code.find(EXTERN_STR));
   }

   //  If the original declaration was deleted and had a comment, and the
   //  definition is not commented, paste the declaration's comment before
   //  the definition after adding a space below it.
   //
   auto& editor = cpp->GetEditor();

   if(!ext && !code.empty() && !ItemIsCommented(defn))
   {
      defn->GetRange(begin, pos, end);
      editor.InsertLine(editor.NextBegin(end), EMPTY_STR);
      editor.Insert(editor.CurrBegin(begin), code);
   }

   //  Remove the class name from the definition.
   //
   pos = editor.LineRfind(defn->GetPos(), qual);
   if(pos != string::npos)
      editor.Erase(pos, qual.size());
   else
      return NotFound("Data definition class qualifier");

   return editor.Changed(pos);
}

//------------------------------------------------------------------------------

word Editor::ChangeDebugFtName(CliThread& cli, const CodeWarning& log)
{
   Debug::ft("Editor.ChangeDebugFtName");

   //  This handles the following warnings for the string passed to Debug::ft:
   //  o DebugFtNameMismatch: the string doesn't start with "Scope.Function"
   //  o DebugFtNameDuplicated: another function already uses the same string
   //
   //  Start by finding the location of the logged Debug::ft invocation.
   //
   auto cpos = Find(log.Pos(), "Debug::ft");
   if(cpos == string::npos) return NotFound("Debug::ft invocation");

   //  Find the location of the first fn_name definition that precedes this
   //  function.  If one is found, it belongs to a previous function if a
   //  right brace appears between it and the start of this function.
   //
   auto fpos = log.item_->GetPos();
   if(fpos == string::npos) return NotFound("Function name");
   auto dpos = Rfind(fpos, "fn_name");

   if(dpos != string::npos)
   {
      auto valid = true;

      for(auto left = dpos; valid && (left < fpos); left = NextBegin(left))
      {
         if(LineFind(left, "}") != string::npos) valid = false;
      }

      if(!valid) dpos = string::npos;
   }

   //  Generate the string (FLIT) and fn_name (FVAR).  If FLIT is already
   //  in use, prompt the user for a unique suffix.
   //
   string flit, fvar, fname;

   DebugFtNames(static_cast< const Function* >(log.item_), flit, fvar);
   if(!EnsureUniqueDebugFtName(cli, flit, fname))
      return Report(FixSkipped);

   if(dpos == string::npos)
   {
      //  An fn_name definition was not found, so the Debug::ft invocation
      //  must have used a string literal.  Replace it.
      //
      auto lpar = FindFirstOf(cpos, "(");
      if(lpar == string::npos) return NotFound("Left parenthesis");
      auto rpar = FindClosing('(', ')', lpar + 1);
      if(rpar == string::npos) return NotFound("Right parenthesis");
      Erase(lpar + 1, rpar - lpar - 1);
      Insert(lpar + 1, fname);
      return Changed(cpos);
   }

   //  The Debug::ft invocation used an fn_name.  It might be used elsewhere
   //  (e.g. for calls to Debug::SwLog), so keep its name and only replace
   //  its definition.
   //
   auto lpos = Find(dpos, QUOTE_STR);
   if(lpos == string::npos) return NotFound("fn_name left quote");
   auto rpos = source_.find(QUOTE, lpos + 1);
   if(rpos == string::npos) return NotFound("fn_name right quote");
   Replace(lpos, rpos - lpos + 1, fname);

   if(LineSize(lpos) - 1 > LineLengthMax())
   {
      auto epos = FindFirstOf(dpos, "=");
      if(epos != string::npos) InsertLineBreak(epos + 1);
   }

   return Changed(lpos);
}

//------------------------------------------------------------------------------

void Editor::ChangeForwards
   (const CxxToken* item, fixed_string from, fixed_string to)
{
   Debug::ft("Editor.ChangeForwards");

   SymbolVector forwards;
   auto syms = Singleton< CxxSymbols >::Instance();

   syms->FindItems(item->Name(), FORW_MASK | FRIEND_MASK, forwards);

   for(auto f = forwards.cbegin(); f != forwards.cend(); ++f)
   {
      if(!(*f)->IsInternal() && ((*f)->Referent() == item))
      {
         auto& editor = (*f)->GetFile()->GetEditor();
         auto pos = (*f)->GetPos();
         auto cpos = editor.Find(pos, from);
         if(cpos == string::npos) continue;
         editor.Replace(pos, strlen(from), to);
      }
   }
}

//------------------------------------------------------------------------------

word Editor::ChangeFunctionToFree(const Function* func)
{
   Debug::ft("Editor.ChangeFunctionToFree");

   //  o If the function is invoked externally, move its declaration to after
   //    its class, into the enclosing namespace, else just erase it.
   //  o In the definition, replace the class name with the namespace in the
   //    fn_name or Debug::ft string literal.  If it uses any static items
   //    from the class, prefix the class name to those items.
   //  o Move the definition to the correct location.
   //
   return Unimplemented();
}

//------------------------------------------------------------------------------

word Editor::ChangeFunctionToMember(const Function* func, word offset)
{
   Debug::ft("Editor.ChangeFunctionToMember");

   //  o Declare the function in the class associated with the argument at
   //    OFFSET, removing that argument.
   //  o Define the function in the correct location, changing the argument
   //    at OFFSET to "this".
   //
   return Unimplemented();
}

//------------------------------------------------------------------------------

word Editor::ChangeInvokerToFree(const Function* func)
{
   Debug::ft("Editor.ChangeInvokerToFree");

   //  Change invokers of this function to invoke it directly instead of
   //  through its class.  An invoker not in the same namespace may have
   //  to prefix the function's namespace.
   //
   return Unimplemented();
}

//------------------------------------------------------------------------------

word Editor::ChangeInvokerToMember(const Function* func, word offset)
{
   Debug::ft("Editor.ChangeInvokerToMember");

   //  Change invokers of this function to invoke it through the argument at
   //  OFFSET instead of directly.  An invoker may need to #include the class's
   //  header.
   //
   return Unimplemented();
}

//------------------------------------------------------------------------------

word Editor::ChangeOperator(const CodeWarning& log)
{
   Debug::ft("Editor.ChangeOperator");

   //  This fixes two different warnings:
   //  o StaticFunctionViaMember: change . or -> to :: and what precedes
   //    the operator to the name of the function's class.
   //  o BitwiseOperatorOnBoolean: replace | with || or & with &&.
   //
   return Unimplemented();
}

//------------------------------------------------------------------------------

word Editor::ChangeSpecialFunction(CliThread& cli, const CodeWarning& log)
{
   Debug::ft("Editor.ChangeSpecialFunction");

   //  This is logged on a class when the function doesn't yet exist.
   //
   if(log.item_->Type() == Cxx::Class)
   {
      return InsertSpecialFunctions(cli, log.item_);
   }

   //  The function already exists, so its access control needs to change.
   //
   auto func = static_cast< const Function* >(log.item_);
   auto cls = func->GetClass();
   ItemDeclAttrs attrs(func);
   word rc = EditFailed;

   switch(log.warning_)
   {
   case PublicConstructor:
      if(cls->IsInstantiated()) return Report(ClassInstantiated);
      attrs.access = Cxx::Protected;
      break;

   case NonVirtualDestructor:
      switch(ChooseDtorAttributes(cli, attrs))
      {
      case EditCompleted:
         return Report("Destructor remains unchanged.", EditCompleted);
      case EditFailed:
         return Report(FixSkipped);
      }

      if(attrs.virt != func->IsVirtual())
      {
         if(attrs.virt)
            rc = TagAsVirtual(log);
         else
            rc = EraseVirtualTag(log);
         if(rc != EditSucceeded) return rc;
      }
      break;

   case ConstructorNotPrivate:
   case DestructorNotPrivate:
      attrs.access = Cxx::Private;
      break;
   default:
      return Report("Internal error: unexpected warning type.");
   }

   auto item = const_cast< CxxToken* >(log.item_);
   return ChangeAccess(item, attrs);
}

//------------------------------------------------------------------------------

word Editor::ChangeStructToClass(const CodeWarning& log)
{
   Debug::ft("Editor.ChangeStructToClass");

   //  Start by changing the structs's forward declarations.
   //
   ChangeForwards(log.item_, STRUCT_STR, CLASS_STR);

   //  Look for the struct's name and then back up to "struct".
   //
   auto pos = log.item_->GetPos();
   if(pos == string::npos) return NotFound("Struct name");
   pos = Rfind(pos, STRUCT_STR);
   if(pos == string::npos) return NotFound(STRUCT_STR, true);
   Replace(pos, strlen(STRUCT_STR), CLASS_STR);

   //  Unless the struct began with a "public:" access control, insert one.
   //
   auto left = Find(pos, "{");
   if(left == string::npos) return NotFound("Left brace");
   auto access = FindWord(left + 1, PUBLIC_STR);
   if(access == string::npos)
   {
      string control(PUBLIC_STR);
      control.push_back(':');
      InsertLine(NextBegin(left), control);
   }
   return Changed(pos);
}

//------------------------------------------------------------------------------

fn_name Editor_CodeBegin = "Editor.CodeBegin";

size_t Editor::CodeBegin()
{
   Debug::ft(Editor_CodeBegin);

   std::vector< size_t > positions;

   auto c = file_->Classes();
   if(!c->empty()) positions.push_back(c->front()->GetPos());

   auto d = file_->Datas();
   if(!d->empty()) positions.push_back(d->front()->GetPos());

   auto e = file_->Enums();
   if(!e->empty()) positions.push_back(e->front()->GetPos());

   auto f = file_->Funcs();
   if(!f->empty()) positions.push_back(f->front()->GetPos());

   auto t = file_->Types();
   if(!t->empty()) positions.push_back(t->front()->GetPos());

   size_t pos = string::npos;

   for(auto p = positions.cbegin(); p != positions.end(); ++p)
   {
      if(*p < pos) pos = *p;
   }

   auto ns = false;

   for(pos = PrevBegin(pos); pos != 0; pos = PrevBegin(pos))
   {
      auto type = PosToType(pos);

      if(!LineTypeAttr::Attrs[type].isCode && (type != FileComment))
      {
         //  Keep moving up the file.  The idea is to stop at an
         //  #include, forward declaration, or using statement.
         //
         continue;
      }

      switch(type)
      {
      case OpenBrace:
         //
         //  This should be the brace for a namespace enclosure because
         //  we started going up the file from the first code item that
         //  could be followed by one (class, enum, or function).
         //
         ns = true;
         break;

      case CodeLine:
         //
         //  If we saw an open brace, this should be a namespace enclosure.
         //  If it is, continue to back up.  If a namespace is expected but
         //  not found, generate a log.  In this case, or when a namespace
         //  *wasn't* expected, assume that the code starts after the line
         //  that we're now on, which is probably a forward declaration.
         //
         if(ns)
         {
            if(LineFind(pos, NAMESPACE_STR) != string::npos) continue;
            Debug::SwLog(Editor_CodeBegin, "namespace expected", pos);
         }
         return NextBegin(pos);

      case AccessControl:
      case DebugFt:
      case FunctionName:
         //
         //  These shouldn't occur.
         //
         Debug::SwLog(Editor_CodeBegin, "unexpected line type", type);
         //  [[fallthrough]]
      case FileComment:
      case CloseBrace:
      case CloseBraceSemicolon:
      case IncludeDirective:
      case HashDirective:
      case UsingStatement:
      default:
         //
         //  We're now one line above what should be the start of the
         //  file's code, plus any relevant comments that precede it.
         //
         return NextBegin(pos);
      }
   }

   return pos;
}

//------------------------------------------------------------------------------

bool Editor::CodeFollowsImmediately(size_t pos) const
{
   Debug::ft("Editor.CodeFollowsImmediately");

   //  Proceed from POS, skipping blank lines and access controls.  Return
   //  false if the next thing is executable code (this excludes braces and
   //  access controls), else return false.
   //
   for(pos = NextBegin(pos); pos != string::npos; pos = NextBegin(pos))
   {
      auto type = PosToType(pos);

      if(LineTypeAttr::Attrs[type].isExecutable) return true;
      if(LineTypeAttr::Attrs[type].isBlank) continue;
      if(type == AccessControl) continue;
      return false;
   }

   return false;
}

//------------------------------------------------------------------------------

bool Editor::Commit(const CliThread& cli)
{
   Debug::ft("Editor.Commit");

   //  Perform an automatic >format on each file.  In particular, some edits
   //  could have introduced blank line pairs.
   //
   auto err = false;

   while(!Editors_.empty())
   {
      string expl;
      auto editor = Editors_.cbegin();
      if((*editor)->Format(expl) != EditCompleted)
         err = true;
      else
         ++Commits_;
      *cli.obuf << spaces(2) << expl;
      expl.clear();
      Editors_.erase(editor);
   }

   return !err;
}

//------------------------------------------------------------------------------

size_t Editor::CommitCount() { return Commits_; }

//------------------------------------------------------------------------------

word Editor::ConvertTabsToBlanks()
{
   Debug::ft("Editor.ConvertTabsToBlanks");

   auto indent = IndentSize();

   //  Run through the source, looking for tabs.
   //
   for(size_t pos = source_.find(TAB); pos != string::npos;
      pos = source_.find(TAB, pos))
   {
      //  Find the start of this line.  If the tab appears in a comment,
      //  ignore it.  Otherwise determine how many spaces to insert when
      //  replacing the tab.
      //
      auto begin = CurrBegin(pos);
      auto end = LineFind(begin, COMMENT_STR);
      if(pos >= end) continue;

      auto count = (pos - begin) % indent;
      if(count == 0) count = indent;
      Erase(pos, 1);
      Insert(pos, spaces(count));
      Changed();
   }

   return EditSucceeded;
}

//------------------------------------------------------------------------------

size_t Editor::CutCode(const CxxToken* item, string& code)
{
   Debug::ft("Editor.CutCode");

   code.clear();

   if(item == nullptr)
   {
      Report("Internal error: no item specified.");
      return string::npos;
   }

   //  Find the where the code to be cut begins and ends.
   //
   auto begin = FindCutBegin(item);
   if(begin == string::npos)
   {
      NotFound("Start of code to be edited");
      return string::npos;
   }

   auto end = begin;

   auto endchars = item->EndChars();
   if(endchars.empty())
   {
      Report("Internal error: item cannot be edited.");
      return string::npos;
   }
   else if(endchars == CRLF_STR)
   {
      end = CurrEnd(begin);
   }
   else
   {
      auto start = begin;

      if((item->Type() == Cxx::Function) &&
         (endchars.find('}') != string::npos))
      {
         //  To find the right brace at the end of a function definition, the
         //  search must start after its initial left brace.
         //
         start = FindFirstOf(begin, "{");
         ++start;
      }

      end = FindFirstOf(start, endchars);
      if(end == string::npos)
      {
         NotFound("End of code to be edited");
         return string::npos;
      }

      //  See if the character that precedes the item should be cut instead of
      //  the one that terminated it.  If cutting that character, replace it
      //  with a space if a comment follows, to keep trailing comments aligned.
      //  Otherwise erase it.  Finally, adjust END to the previous character or,
      //  if it is the first non-blank character on its line, to the end of the
      //  previous line.
      //
      auto beginchars = item->BeginChars(source_[end]);

      if(!beginchars.empty())
      {
         if(beginchars[0] != '$')
         {
            auto prev = RfindFirstOf(begin - 1, beginchars);

            if(FindComment(prev) != string::npos)
            {
               source_[prev] = SPACE;
            }
            else
            {
               Erase(prev, 1);
               --begin;
               --end;
            }

            if(IsFirstNonBlank(end))
               end = CurrBegin(end) - 1;
            else
               --end;
         }
         else
         {
            //  This cuts from the start of ITEM to END, along with any
            //  spaces that follow END.
            //
            begin = item->GetPos();
            end = source_.find_first_not_of(WhitespaceChars, end + 1) - 1;
         }
      }

      //  When the code ends at a right brace, also cut any semicolon that
      //  immediately follows.
      //
      if(source_[end] == '}')
      {
         Reposition(end + 1);
         if(CurrChar() == ';') end = Curr();
      }

      //  Cut any comment or whitespace that follows on the last line.
      //
      if(NoCodeFollows(end + 1)) end = CurrEnd(end);
   }

   //  If entire lines of code that aren't immediately followed by more code
   //  are being cut, also cut any comment that precedes the code, since it
   //  almost certainly refers only to the code being cut.  But don't do this
   //  for a preprocessor directive (endchars == CRLF_STR).
   //
   if((begin == CurrBegin(begin)) && (end == CurrEnd(end)))
   {
      if(!CodeFollowsImmediately(end) && (endchars != CRLF_STR))
      {
         begin = IntroStart(begin, false);
      }
   }

   //  Extract the code bounded by [begin, end].  If BEGIN and END are not
   //  on the same line, then
   //  o if BEGIN is not at the start of its line, insert a CRLF before it
   //  o if END is not at the end of its line, insert a CRLF after it (and
   //    include it in the code) and re-indent that line
   //
   if(!OnSameLine(begin, end))
   {
      if(source_[begin - 1] != CRLF)
      {
         Insert(begin, CRLF_STR);
         ++begin;
         ++end;
      }

      if(source_[end] != CRLF)
      {
         auto indent = CRLF_STR + spaces(end - CurrBegin(end));
         Insert(end + 1, indent);
         ++end;
      }
   }

   code = source_.substr(begin, end - begin + 1);
   Erase(begin, end - begin + 1);
   return begin;
}

//------------------------------------------------------------------------------

string Editor::DebugFtCode(const string& fname) const
{
   Debug::ft("Editor.DebugFtCode");

   auto call = string(IndentSize(), SPACE) + "Debug::ft(";
   call.append(fname);
   call.append(");");
   return call;
}

//------------------------------------------------------------------------------

void Editor::Display(ostream& stream,
   const string& prefix, const Flags& options) const
{
   Lexer::Display(stream, prefix, options);

   stream << prefix << "file     : " <<
      (file_ != nullptr ? file_->Name() : "no file specified") << CRLF;
   stream << CRLF;
   stream << prefix << "sorted   : " << sorted_ << CRLF;
   stream << prefix << "aliased  : " << aliased_ << CRLF;
   stream << prefix << "warnings : " << warnings_.size() << CRLF;
}

//------------------------------------------------------------------------------

bool Editor::DisplayLog
   (const CliThread& cli, const CodeWarning& log, bool file) const
{
   Debug::ft("Editor.DisplayLog");

   if(file)
   {
      *cli.obuf << log.File()->Name() << ':' << CRLF;
   }

   //  Display LOG's details.
   //
   *cli.obuf << "  Line " << log.Line() + 1;
   if(log.offset_ > 0) *cli.obuf << '/' << log.offset_;
   *cli.obuf << ": " << Warning(log.warning_);
   if(log.HasInfoToDisplay()) *cli.obuf << ": " << log.info_;
   *cli.obuf << CRLF;

   if(log.HasCodeToDisplay())
   {
      //  Display the current version of the code associated with LOG.
      //
      *cli.obuf << spaces(2);
      auto code = GetCode(log.Pos());

      if(code.empty())
      {
         *cli.obuf << "Code not found." << CRLF;
         return false;
      }

      if(code.find_first_not_of(WhitespaceChars) == string::npos)
      {
         *cli.obuf << "[line contains only whitespace]" << CRLF;
         return true;
      }

      *cli.obuf << DemangleInclude(code);
   }

   return true;
}

//------------------------------------------------------------------------------

size_t Editor::Erase(size_t pos, size_t count)
{
   Debug::ft("Editor.Erase");

   if(count == 0) return pos;
   source_.erase(pos, count);
   lastCut_ = pos;
   Update();
   file_->UpdatePos(Erased, pos, count);
   UpdateWarnings(Erased, pos, count);
   Changed();
   return pos;
}

//------------------------------------------------------------------------------

word Editor::EraseAccessControl(const CodeWarning& log)
{
   Debug::ft("Editor.EraseAccessControl");

   //  The parser logs RedundantAccessControl at the position
   //  where it occurred; log.item_ is nullptr in this warning.
   //
   auto begin = CurrBegin(log.Pos());
   if(begin == string::npos) return NotFound("Access control position");
   size_t len = 0;

   //  Look for the access control keyword and note its length.
   //
   auto access = LineFind(begin, PUBLIC_STR);

   while(true)
   {
      if(access != string::npos)
      {
         len = strlen(PUBLIC_STR);
         break;
      }

      access = LineFind(begin, PROTECTED_STR);

      if(access != string::npos)
      {
         len = strlen(PROTECTED_STR);
         break;
      }

      access = LineFind(begin, PRIVATE_STR);

      if(access != string::npos)
      {
         len = strlen(PRIVATE_STR);
         break;
      }

      return NotFound("Access control keyword");
   }

   //  Look for the colon that follows the keyword.
   //
   auto colon = FindNonBlank(access + len);
   if((colon == string::npos) || (source_[colon] != ':'))
      return NotFound("Colon after access control");

   //  Erase the keyword and colon.
   //
   Erase(colon, 1);
   Erase(access, len);
   return Changed(access);
}

//------------------------------------------------------------------------------

word Editor::EraseAdjacentSpaces(const CodeWarning& log)
{
   Debug::ft("Editor.EraseAdjacentSpaces");

   auto begin = CurrBegin(log.Pos());
   if(begin == string::npos) return NotFound("Line with adjacent spaces");
   auto pos = LineFindFirst(begin);
   if(pos == string::npos) return NotFound("Adjacent spaces");

   //  If this line has a trailing comment that is aligned with one on the
   //  previous or the next line, keep the comments aligned by moving the
   //  erased spaces immediately to the left of the comment.
   //
   auto move = false;
   auto cpos = FindComment(pos);

   if(cpos != string::npos)
   {
      cpos -= begin;

      if(pos != begin)
      {
         auto prev = PrevBegin(begin);
         if(prev != string::npos)
         {
            move = (cpos == (FindComment(prev) - prev));
         }
      }

      if(!move)
      {
         auto next = NextBegin(begin);
         if(next != string::npos)
         {
            move = (cpos == (FindComment(next) - next));
         }
      }
   }

   //  Don't erase adjacent spaces that precede a trailing comment.
   //
   auto stop = cpos;

   if(stop != string::npos)
      while(IsBlank(source_[stop - 1])) --stop;
   else
      stop = CurrEnd(begin);

   cpos = stop;  // (comm - stop) will be number of erased spaces

   while(pos + 1 < stop)
   {
      if(IsBlank(source_[pos]) && IsBlank(source_[pos + 1]))
      {
         Erase(pos, 1);
         --stop;
      }
      else
      {
         ++pos;
      }
   }

   if(move) Insert(stop, string(cpos - stop, SPACE));
   return Changed(begin);
}

//------------------------------------------------------------------------------

word Editor::EraseArgument(const Function* func, word offset)
{
   Debug::ft("Editor.EraseArgument");

   //  In this function invocation, erase the argument at OFFSET.
   //
   return Unimplemented();
}

//------------------------------------------------------------------------------

word Editor::EraseBlankLine(const CodeWarning& log)
{
   Debug::ft("Editor.EraseBlankLine");

   //  Remove the specified line of code.
   //
   EraseLine(log.Pos());
   return Changed();
}

//------------------------------------------------------------------------------

word Editor::EraseClass(const CodeWarning& log)
{
   Debug::ft("Editor.EraseClass");

   //  Erase the class's definition and the definitions of its functions
   //  and static data.
   //
   return Unimplemented();
}

//------------------------------------------------------------------------------

word Editor::EraseCode(const CxxToken* item)
{
   Debug::ft("Editor.EraseCode");

   string code;
   if(CutCode(item, code) != string::npos) return Changed();
   return EditFailed;
}

//------------------------------------------------------------------------------

word Editor::EraseConst(const CodeWarning& log)
{
   Debug::ft("Editor.EraseConst");

   //  There are two places for a redundant "const" after the typename:
   //    "const" <typename> "const" [<typetags>] "const"
   //  The log indicates the position of the redundant "const".  If there is
   //  more than one of them, each one causes a log, so we can erase them one
   //  at a time.  To ensure that the wrong "const" is not erased after other
   //  edits have occurred on this line, verify that the const is not preceded
   //  by certain punctuation.  A preceding '(' or ',' can only occur if the
   //  "const" precedes its typename, and a '*' can only occur if the "const"
   //  makes a pointer const.
   //
   for(auto pos = FindWord(log.Pos(), CONST_STR); pos != string::npos;
      pos = FindWord(pos + 1, CONST_STR))
   {
      auto prev = RfindNonBlank(pos - 1);

      //  This is the first non-blank character preceding a subsequent
      //  "const".  If it's a pointer tag, it makes the pointer const,
      //  so continue with the next "const".
      //
      switch(source_[prev])
      {
      case '*':
      case ',':
      case '(':
         continue;
      }

      //  This is the redundant const, so erase it.  Also erase a space
      //  between it and the previous non-blank character.
      //
      Erase(pos, strlen(CONST_STR));

      if(OnSameLine(prev, pos) && (pos - prev > 1))
      {
         Erase(prev + 1, 1);
      }

      return Changed(pos);
   }

   return NotFound("Redundant const");
}

//------------------------------------------------------------------------------

word Editor::EraseData(const CliThread& cli, const CodeWarning& log)
{
   Debug::ft("Editor.EraseData");

   auto decl = static_cast< const Data* >(log.item_);
   auto defn = decl->GetMate();
   auto& refs = decl->Xref();

   //  Erase any references to the data.
   //
   for(auto r = refs.cbegin(); r != refs.cend(); ++r)
   {
      if(AreInSameStatement(decl, *r)) continue;
      if(AreInSameStatement(defn, *r)) continue;

      auto file = (*r)->GetFile();
      auto& editor = file->GetEditor();

      if(editor.EraseCode(*r) != EditSucceeded)
      {
         auto expl = GetExpl();

         if(!expl.empty())
         {
            if(expl.back() == CRLF) expl.pop_back();
            expl += " (" + (*r)->strLocation() + ')';
            *cli.obuf << spaces(2) << expl << CRLF;
         }
      }
   }

   //  Erase the data definition, if any.
   //
   if((defn != nullptr) && (defn != decl))
   {
      auto& editor = defn->GetFile()->GetEditor();

      if(editor.EraseCode(defn) != EditSucceeded)
      {
         auto expl = GetExpl();

         if(!expl.empty())
         {
            if(expl.back() == CRLF) expl.pop_back();
            expl += " (definition)";
            *cli.obuf << spaces(2) << expl << CRLF;
         }
      }
   }

   //  Erase the data declaration.
   //
   return EraseCode(log.item_);
}

//------------------------------------------------------------------------------

word Editor::EraseDefaultValue(const Function* func, word offset)
{
   Debug::ft("Editor.EraseDefaultValue");

   //  Erase this argument's default value.
   //
   return Unimplemented();
}

//------------------------------------------------------------------------------

word Editor::EraseEmptyNamespace(size_t pos)
{
   Debug::ft("Editor.EraseEmptyNamespace");

   //  POS is the character after a forward declaration that was just
   //  deleted.  If this left an empty "namespace <ns> { }", remove it,
   //  else just report success.
   //
   if(pos == string::npos) return EditSucceeded;
   if(!CodeMatches(pos, "}")) return EditSucceeded;

   auto p1 = PrevBegin(pos);
   if(p1 == 0) return EditSucceeded;
   auto p2 = PrevBegin(p1);

   if(CodeMatches(p2, NAMESPACE_STR) && (source_[p1] == '{'))
   {
      auto end = CurrEnd(pos);
      Erase(p2, end - p2 + 1);
      return Changed();
   }

   return EditSucceeded;
}

//------------------------------------------------------------------------------

word Editor::EraseExplicitTag(const CodeWarning& log)
{
   Debug::ft("Editor.EraseExplicitTag");

   auto ctor = log.item_->GetPos();
   if(ctor == string::npos) return NotFound("Constructor");
   auto exp = Rfind(ctor, EXPLICIT_STR);
   if(exp == string::npos) return NotFound(EXPLICIT_STR, true);
   Erase(exp, strlen(EXPLICIT_STR) + 1);
   return Changed(exp);
}

//------------------------------------------------------------------------------

word Editor::EraseForward(const CodeWarning& log)
{
   Debug::ft("Editor.EraseForward");

   //  Erasing the forward declaration may leave an empty enclosing
   //  namespace that should be deleted.
   //
   string code;
   auto pos = CutCode(log.item_, code);
   if(pos == string::npos) return EditFailed;
   Changed();
   return EraseEmptyNamespace(pos);
}

//------------------------------------------------------------------------------

size_t Editor::EraseLine(size_t pos)
{
   Debug::ft("Editor.EraseLine");

   auto begin = CurrBegin(pos);
   auto end = CurrEnd(pos);
   Erase(begin, end - begin + 1);
   return begin;
}

//------------------------------------------------------------------------------

bool Editor::EraseLineBreak(size_t pos)
{
   Debug::ft("Editor.EraseLineBreak(pos)");

   auto curr = CurrBegin(pos);
   if(curr == string::npos) return false;
   auto next = NextBegin(curr);
   if(next == string::npos) return false;

   //  Check that the lines can be merged.
   //
   auto type = PosToType(curr);
   if(!LineTypeAttr::Attrs[type].isMergeable) return false;
   type = PosToType(next);
   if(!LineTypeAttr::Attrs[type].isMergeable) return false;
   auto size = LineMergeLength
      (GetCode(curr), 0, LineSize(curr) - 1,
       GetCode(next), 0, LineSize(next) - 1);
   if(size > LineLengthMax()) return false;

   //  Merge the lines after replacing or erasing CURR's endline.
   //
   auto code1 = source_.substr(curr, LineSize(curr) - 1);
   auto code2 = source_.substr(next, LineSize(next));
   auto start = LineFindFirst(next);

   if(InsertSpaceOnMerge(code1, code2, start - next))
   {
      Replace(next - 1, 1, SPACE_STR);
      Erase(next, start - next);
   }
   else
   {
      Erase(next - 1, start - next + 1);
   }

   return true;
}

//------------------------------------------------------------------------------

word Editor::EraseLineBreak(const CodeWarning& log)
{
   Debug::ft("Editor.EraseLineBreak(log)");

   auto begin = CurrBegin(log.Pos());
   if(begin == string::npos) return NotFound("Position for line break");
   auto merged = EraseLineBreak(begin);
   if(!merged) return Report("Line break was not removed.");
   return Changed(begin);
}

//------------------------------------------------------------------------------

word Editor::EraseMutableTag(const CodeWarning& log)
{
   Debug::ft("Editor.EraseMutableTag");

   //  Find the line on which the data's type appears, and erase the
   //  "mutable" before that type.
   //
   auto type = log.item_->GetTypeSpec()->GetPos();
   if(type == string::npos) return NotFound("Data type");
   auto tag = Rfind(type, MUTABLE_STR);
   if(tag == string::npos) return NotFound(MUTABLE_STR, true);
   Erase(tag, strlen(MUTABLE_STR) + 1);
   return Changed(tag);
}

//------------------------------------------------------------------------------

word Editor::EraseNoexceptTag(const Function* func)
{
   Debug::ft("Editor.EraseNoexceptTag");

   //  Look for "noexcept" just before the end of the function's signature.
   //
   auto endsig = FindSigEnd(func);
   if(endsig == string::npos) return NotFound("Signature end");
   endsig = Rfind(endsig, NOEXCEPT_STR);
   if(endsig == string::npos) return NotFound(NOEXCEPT_STR, true);
   size_t space = (IsFirstNonBlank(endsig) ? 0 : 1);
   Erase(endsig - space, strlen(NOEXCEPT_STR) + space);
   return Changed(endsig);
}

//------------------------------------------------------------------------------

word Editor::EraseOverrideTag(const CodeWarning& log)
{
   Debug::ft("Editor.EraseOverrideTag");

   //  Look for "override" just before the end of the function's signature.
   //
   auto endsig = FindSigEnd(log);
   if(endsig == string::npos) return NotFound("Signature end");
   endsig = Rfind(endsig, OVERRIDE_STR);
   if(endsig == string::npos) return NotFound(OVERRIDE_STR, true);
   size_t space = (IsFirstNonBlank(endsig) ? 0 : 1);
   Erase(endsig - space, strlen(OVERRIDE_STR) + space);
   return Changed(endsig);
}

//------------------------------------------------------------------------------

word Editor::EraseParameter(const Function* func, word offset)
{
   Debug::ft("Editor.EraseParameter");

   //  Erase the argument at OFFSET in this function definition or declaration.
   //
   return Unimplemented();
}

//------------------------------------------------------------------------------

word Editor::EraseScope(const CodeWarning& log)
{
   Debug::ft("Editor.EraseScope");

   auto begin = log.item_->GetPos();
   if(begin == string::npos) return NotFound("Qualified name");
   auto op = source_.find(SCOPE_STR, begin);
   if(op == string::npos) return NotFound("Scope resolution operator");
   Erase(begin, op - begin + 2);
   return Changed(begin);
}

//------------------------------------------------------------------------------

word Editor::EraseSemicolon(const CodeWarning& log)
{
   Debug::ft("Editor.EraseSemicolon");

   //  The parser logs a redundant semicolon that follows the closing '}'
   //  of a function definition or namespace enclosure.
   //
   auto begin = CurrBegin(log.Pos());
   if(begin == string::npos) return NotFound("Position of semicolon");
   auto semi = FindFirstOf(begin, ";");
   if(semi == string::npos) return NotFound("Semicolon");
   auto brace = RfindNonBlank(semi - 1);
   if(brace == string::npos) return NotFound("Right brace");
   if(source_[brace] != '}') return NotFound("Right brace");
   Erase(semi, 1);
   return Changed(semi);
}

//------------------------------------------------------------------------------

word Editor::EraseTrailingBlanks()
{
   Debug::ft("Editor.EraseTrailingBlanks");

   for(size_t begin = 0; begin != string::npos; begin = NextBegin(begin))
   {
      auto end = CurrEnd(begin);
      if(begin == end) continue;

      auto pos = end - 1;
      while(IsBlank(source_[pos]) && (pos >= begin)) --pos;

      if(pos < end - 1)
      {
         Erase(pos + 1, end - pos - 1);
         Changed();
      }
   }

   return EditSucceeded;
}

//------------------------------------------------------------------------------

word Editor::EraseVirtualTag(const CodeWarning& log)
{
   Debug::ft("Editor.EraseVirtualTag");

   //  Look for "virtual" just before the function's return type.
   //
   auto type = log.item_->GetTypeSpec()->GetPos();
   if(type == string::npos) return NotFound("Function type");
   auto virt = LineRfind(type, VIRTUAL_STR);
   if(virt == string::npos) return NotFound(VIRTUAL_STR, true);
   Erase(virt, strlen(VIRTUAL_STR) + 1);
   return Changed(virt);
}

//------------------------------------------------------------------------------

word Editor::EraseVoidArgument(const CodeWarning& log)
{
   Debug::ft("Editor.EraseVoidArgument");

   //  The function might return "void", so the second occurrence of "void"
   //  could be the argument.  Erase it, leaving only the parentheses.
   //
   auto begin = CurrBegin(log.Pos());
   if(begin == string::npos) return NotFound("Position of void argument");

   for(auto arg = FindWord(begin, VOID_STR); arg != string::npos;
      arg = FindWord(arg + 1, VOID_STR))
   {
      auto lpar = RfindNonBlank(arg - 1);
      if(lpar == string::npos) continue;
      if(source_[lpar] != '(') continue;
      auto rpar = FindNonBlank(arg + strlen(VOID_STR));
      if(rpar == string::npos) break;
      if(source_[rpar] != ')') continue;
      if(OnSameLine(arg, lpar) && OnSameLine(arg, rpar))
      {
         Erase(lpar + 1, rpar - lpar - 1);
         return Changed(lpar);
      }
      Erase(arg, strlen(VOID_STR));
      return Changed(arg);
   }

   return NotFound(VOID_STR, true);
}

//------------------------------------------------------------------------------

size_t Editor::FindAndCutInclude(size_t pos, const string& incl)
{
   Debug::ft("Editor.FindAndCutInclude");

   for(NO_OP; pos != string::npos; pos = NextBegin(pos))
   {
      if(GetCode(pos) == incl)
      {
         return EraseLine(pos);
      }
   }

   return string::npos;
}

//------------------------------------------------------------------------------

size_t Editor::FindArgsEnd(const Function* func)
{
   Debug::ft("Editor.FindArgsEnd");

   auto name = func->GetPos();
   if(name == string::npos) return string::npos;
   auto lpar = FindFirstOf(name, "(");
   if(lpar == string::npos) return string::npos;
   auto rpar = FindClosing('(', ')', lpar + 1);
   return rpar;
}

//------------------------------------------------------------------------------

size_t Editor::FindCutBegin(const CxxToken* item) const
{
   Debug::ft("Editor.FindCutBegin");

   //  Unless the line contains multiple items, cut starting at its beginning.
   //  If there are multiple items, cut after the last delimiter before ITEM.
   //  A scope resolution operator does not qualify as a delimiter.
   //
   auto targ = item->GetPos();
   size_t pos = CurrBegin(targ);

   for(size_t next = pos; NO_OP; ++next)
   {
      next = source_.find_first_of(",(:;{}", next);
      if(next >= targ) break;
      if(CodeMatches(next, SCOPE_STR))
         ++next;
      else
         pos = next + 1;
   }

   return pos;
}

//------------------------------------------------------------------------------

word Editor::FindFuncDefnLoc(const CodeFile* file, const Class* cls,
   const string& name, FuncDefnAttrs& attrs) const
{
   Debug::ft("Editor.FindFuncDefnLoc(name)");

   //  Look at all the functions that are defined in this file and that belong
   //  to CLS.
   //
   auto funcs = file->Funcs();
   const Function* prev = nullptr;
   const Function* next = nullptr;
   auto reached = false;

   for(auto f = funcs->cbegin(); f != funcs->cend(); ++f)
   {
      //  Ignore instantiated functions.
      //
      if((*f)->IsInTemplateInstance()) continue;

      //  If the current function is in another class, insert the new function
      //  o immediately before it, if the new function's class has been reached
      //  o somewhere after it, if the new function's class has not been reached
      //
      if((*f)->GetClass() != cls)
      {
         if(reached)
         {
            next = (*f)->GetDefn();
            break;
         }

         prev = (*f)->GetDefn();
         continue;
      }

      //  The current function is in the same class.  Insert the new function
      //  in the cardinal order defined by FunctionRole.  When the roles match,
      //  insert the new function alphabetically.
      //
      reached = true;
      auto currRole = (*f)->FuncRole();

      if(attrs.role < currRole)
      {
         next = (*f)->GetDefn();
         break;
      }
      else if(attrs.role > currRole)
      {
         prev = (*f)->GetDefn();
         continue;
      }

      auto currName = (*f)->Name();
      auto sort = currName.compare(name);

      if(sort > 0)
      {
         next = (*f)->GetDefn();
         break;
      }
      else if(sort < 0)
      {
         prev = (*f)->GetDefn();
         continue;
      }
      else
      {
         return Report("A definition for this function already exists.");
      }
   }

   //  We now know the functions between which the new function should be
   //  inserted (PREV and NEXT), so find its precise insertion location
   //  and attributes.
   //
   return UpdateFuncDefnLoc(prev, next, attrs);
}

//------------------------------------------------------------------------------

word Editor::FindItemDeclLoc
   (const Class* cls, const string& name, ItemDeclAttrs& attrs) const
{
   Debug::ft("Editor.FindItemDeclLoc");

   auto where = attrs.CalcDeclOrder();
   CxxNamedVector items;
   GetItems(cls, items);
   const CxxToken* prev = nullptr;
   const CxxToken* next = nullptr;

   for(auto i = items.cbegin(); i != items.cend(); ++i)
   {
      ItemDeclAttrs currAttrs(*i);
      auto order = currAttrs.CalcDeclOrder();

      if(where < order)
      {
         next = *i;
         break;
      }
      else if(where == order)
      {
         if(strCompare((*i)->Name(), name) > 0)
         {
            next = *i;
            break;
         }
      }

      prev = *i;
   }

   //  We now know the items between which the new item should be inserted
   //  (PREV and NEXT), so update its insertion attributes.
   //
   return UpdateItemDeclLoc(cls, prev, next, attrs);
}

//------------------------------------------------------------------------------

CodeWarning* Editor::FindLog(Warning warning, const CxxToken* item, word offset)
{
   Debug::ft("Editor.FindLog");

   for(auto w = warnings_.begin(); w != warnings_.end(); ++w)
   {
      if(((*w)->warning_ == warning) && ((*w)->item_ == item) &&
         ((*w)->offset_ == offset))
      {
         return *w;
      }
   }

   return nullptr;
}

//------------------------------------------------------------------------------

size_t Editor::FindSigEnd(const CodeWarning& log)
{
   Debug::ft("Editor.FindSigEnd(log)");

   if(log.item_ == nullptr) return string::npos;
   if(log.item_->Type() != Cxx::Function) return string::npos;
   return FindSigEnd(static_cast< const Function* >(log.item_));
}

//------------------------------------------------------------------------------

size_t Editor::FindSigEnd(const Function* func)
{
   Debug::ft("Editor.FindSigEnd(func)");

   //  Look for the first semicolon or left brace after the function's name.
   //
   return FindFirstOf(func->GetPos(), ";{");
}

//------------------------------------------------------------------------------

word Editor::FindSpecialFuncDeclLoc
   (CliThread& cli, const Class* cls, ItemDeclAttrs& attrs) const
{
   Debug::ft("Editor.FindSpecialFuncDeclLoc");

   auto base = cls->IsBaseClass();
   auto inst = cls->IsInstantiated();
   auto solo = cls->IsSingleton();
   auto prompt = true;
   Function* prototype = cls->FindFuncByRole(PureDtor, false);
   if(prototype == nullptr) prototype = cls->FindFuncByRole(CopyCtor, false);
   if(prototype == nullptr) prototype = cls->FindFuncByRole(CopyOper, false);

   //  Update ATTRS based on what function is being inserted.
   //
   switch(attrs.role)
   {
   case PureCtor:
      if(solo)
         attrs.access = Cxx::Private;
      else if(base && !inst)
         attrs.access = Cxx::Protected;
      prompt = false;
      prototype = nullptr;
      break;

   case CopyCtor:
      if(solo)
      {
         attrs.deleted = true;
      }
      else
      {
         if(base && !inst) attrs.access = Cxx::Protected;
      }
      break;

   case CopyOper:
      if(solo)
      {
         attrs.deleted = true;
      }
      else
      {
         if(base && !inst) attrs.access = Cxx::Protected;
      }
      break;

   case PureDtor:
      if(base) attrs.virt = true;
      if(solo) attrs.access = Cxx::Private;
      prompt = false;
      break;
   };

   //  The user can decide to define the function as defaulted or deleted.
   //  If it will be defaulted and a related function is not trivial, ask
   //  if the user wants to create a shell for implmenting the function.
   //
   if(prompt && !attrs.deleted)
   {
      *cli.obuf << "Define the " << attrs.role << ". " << CRLF;
      auto response = cli.IntPrompt(DefnPrompt, 0, 2);
      if(response == 0) return Report(FixSkipped);
      attrs.deleted = (response == 2);
   }

   if(attrs.deleted)
   {
      attrs.access = Cxx::Public;
   }
   else
   {
      if((prototype != nullptr) && !prototype->IsTrivial())
      {
         *cli.obuf << "The " << prototype->FuncRole();
         *cli.obuf << " is not trivial." << CRLF;
         attrs.shell = cli.BoolPrompt(ShellPrompt);
      }
   }

   return FindItemDeclLoc(cls, EMPTY_STR, attrs);
}

//------------------------------------------------------------------------------

CxxNamedSet Editor::FindUsingReferents(CxxToken* item) const
{
   Debug::ft("Editor.FindUsingReferents");

   CxxUsageSets symbols;
   CxxNamedSet refs;

   item->GetUsages(*file_, symbols);

   for(auto u = symbols.users.cbegin(); u != symbols.users.cend(); ++u)
   {
      refs.insert((*u)->DirectType());
   }

   return refs;
}

//------------------------------------------------------------------------------

word Editor::Fix(CliThread& cli, const FixOptions& opts, string& expl)
{
   Debug::ft("Editor.Fix");

   //  Run through all the warnings.
   //
   word rc = 0;
   auto reply = 'y';
   auto found = false;
   auto fixed = false;
   auto first = true;
   auto exit = false;

   for(auto item = warnings_.begin(); item != warnings_.end(); ++item)
   {
      //  Skip this item if the user didn't include its warning type.
      //
      if((opts.warning != AllWarnings) &&
         (opts.warning != (*item)->warning_)) continue;

      switch(FixStatus(**item))
      {
      case NotFixed:
         //
         //  This item is eligible for fixing, and note that such an item
         //  was found.
         //
         found = true;
         break;

      case Fixed:
      case Pending:
         //
         //  Before skipping over an eligible item that was already fixed,
         //  note that such an item was found.
         //
         fixed = true;
         continue;

      case NotSupported:
      default:
         //
         //  If multiple warning types are being fixed, try the next one.  If
         //  only one warning type was selected, there will be nothing to fix,
         //  so return a value that will terminate the >fix command.
         //
         if(opts.warning == AllWarnings) continue;
         return Report(NotImplemented, EditAbort);
      }

      //  This item is eligible for fixing.  Display it.
      //
      if(DisplayLog(cli, **item, first))
      {
         first = false;

         //  See if the user wishes to fix the item.  Valid responses are
         //  o 'y' = fix it, which invokes the appropriate function
         //  o 'n' = don't fix it
         //  o 's' = skip this file
         //  o 'q' = done fixing warnings
         //
         expl.clear();
         reply = 'y';

         if(opts.prompt)
         {
            reply = cli.CharPrompt(FixPrompt, YNSQChars, YNSQHelp);
         }

         switch(reply)
         {
         case 'y':
         {
            auto logs = (*item)->LogsToFix(expl);

            if(!expl.empty())
            {
               *cli.obuf << spaces(2) << expl << CRLF;
               expl.clear();
            }

            for(auto log = logs.begin(); log != logs.end(); ++log)
            {
               auto& editor = (*log)->File()->GetEditor();
               rc = editor.FixLog(cli, **log);
               ReportFix(cli, *log, rc);
            }

            break;
         }

         case 'n':
            break;

         case 's':
         case 'q':
            exit = true;
            break;

         default:
            return Report("Internal error: unknown response.", -6);
         }
      }

      cli.Flush();
      if(!opts.prompt) ThisThread::Pause(Duration(20, mSECS));
      if(exit || (rc < EditFailed)) break;
   }

   if(found)
   {
      if(exit || (rc < EditFailed))
         *cli.obuf << spaces(2) << "Remaining warnings skipped." << CRLF;
      else
         *cli.obuf << spaces(2) << "End of warnings." << CRLF;
   }
   else if(fixed)
   {
      *cli.obuf << spaces(2) << "Selected warning(s) in ";
      *cli.obuf << file_->Name() << " previously fixed." << CRLF;
   }
   else
   {
      if(!opts.multiple)
      {
         *cli.obuf << "No warnings that can be fixed were found." << CRLF;
      }
   }

   //  Returning -1 or greater indicates that the next file can still be
   //  processed, so return a lower value if the user wants to quit or if
   //  a serious error occurred.  On a quit, a "committed" message has been
   //  displayed for the last file written, and it will be displayed again
   //  (because of rc < 0) if EPXL is not cleared.
   //
   if(!Commit(cli))
   {
      rc = -6;
   }
   else if((reply == 'q') && (rc >= EditFailed))
   {
      expl.clear();
      rc = -2;
   }

   return rc;
}

//------------------------------------------------------------------------------

word Editor::FixFunction(const Function* func, const CodeWarning& log)
{
   Debug::ft("Editor.FixFunction");

   switch(log.warning_)
   {
   case ArgumentUnused:
      return EraseParameter(func, log.offset_);
   case FunctionUnused:
      return EraseCode(func);
   case VirtualAndPublic:
      return SplitVirtualFunction(func);
   case VirtualDefaultArgument:
      return EraseDefaultValue(func, log.offset_);
   case ArgumentCouldBeConstRef:
      return TagAsConstReference(func, log.offset_);
   case ArgumentCouldBeConst:
      return TagAsConstArgument(func, log.offset_);
   case FunctionCouldBeConst:
      return TagAsConstFunction(func);
   case FunctionCouldBeStatic:
      return TagAsStaticFunction(func);
   case FunctionCouldBeFree:
      return ChangeFunctionToFree(func);
   case FunctionCouldBeDefaulted:
      return TagAsDefaulted(func);
   case CouldBeNoexcept:
      return TagAsNoexcept(func);
   case ShouldNotBeNoexcept:
      return EraseNoexceptTag(func);
   case FunctionCouldBeMember:
      return ChangeFunctionToMember(func, log.offset_);
   }

   return Report("Internal error: unsupported function warning.");
}

//------------------------------------------------------------------------------

word Editor::FixFunctions(CliThread& cli, CodeWarning& log)
{
   Debug::ft("Editor.FixFunctions");

   if(log.item_->Type() != Cxx::Function)
   {
      return Report("Internal error: warning is not for a function.");
   }

   //  Create a list of all the function declarations and definitions that
   //  are associated with the log.
   //
   auto func = static_cast< const Function* >(log.item_);
   std::vector< const Function* > funcs;
   GetOverrides(func, funcs);

   for(auto f = funcs.cbegin(); f != funcs.cend(); ++f)
   {
      auto file = (*f)->GetFile();
      auto& editor = file->GetEditor();
      auto rc = editor.FixFunction(*f, log);
      auto expl = GetExpl();
      auto fn = file->Name();

      if(expl.empty())
      {
         fn += ": ";
         expl = fn + SuccessExpl;
      }
      else
      {
         fn += ":\n" + spaces(4);
         expl = fn + expl;
      }

      ReportFix(cli, &log, rc);
   }

   return EditCompleted;
}

//------------------------------------------------------------------------------

word Editor::FixInvoker(const Function* func, const CodeWarning& log)
{
   Debug::ft("Editor.FixInvoker");

   switch(log.warning_)
   {
   case ArgumentUnused:
      return EraseArgument(func, log.offset_);
   case VirtualDefaultArgument:
      return InsertArgument(func, log.offset_);
   case FunctionCouldBeFree:
      return ChangeInvokerToFree(func);
   case FunctionCouldBeMember:
      return ChangeInvokerToMember(func, log.offset_);
   }

   return Report("Internal error: unexpected invoker warning.");
}

//------------------------------------------------------------------------------

word Editor::FixInvokers(CliThread& cli, const CodeWarning& log)
{
   Debug::ft("Editor.FixInvokers");

   //  Use FixFunctions to modify all of the function signatures, and then
   //  use the cross-reference to find and modify all of the invocations.
   //
   return Unimplemented();
}

//------------------------------------------------------------------------------

word Editor::FixLog(CliThread& cli, CodeWarning& log)
{
   Debug::ft("Editor.FixLog");

   switch(log.status_)
   {
   case NotSupported:
      return Report(NotImplemented);
   case Fixed:
   case Pending:
      return Report("This warning has already been fixed.");
   }

   return FixWarning(cli, log);
}

//------------------------------------------------------------------------------

WarningStatus Editor::FixStatus(const CodeWarning& log) const
{
   Debug::ft("Editor.FixStatus");

   if((log.warning_ == IncludeNotSorted) ||
      (log.warning_ == IncludeFollowsCode))
   {
      //  If there are multiple warnings for unsorted or embedded #include
      //  directives, they all get fixed when the first one gets fixed.
      //
      if(sorted_ && (log.status_ == NotFixed)) return Fixed;
   }

   return log.status_;
}

//------------------------------------------------------------------------------

word Editor::FixWarning(CliThread& cli, CodeWarning& log)
{
   Debug::ft("Editor.FixWarning");

   switch(log.warning_)
   {
   case UseOfNull:
      return ReplaceNull(log);
   case PtrTagDetached:
      return AdjustTags(log);
   case RefTagDetached:
      return AdjustTags(log);
   case RedundantSemicolon:
      return EraseSemicolon(log);
   case RedundantConst:
      return EraseConst(log);
   case DefineNotAtFileScope:
      return MoveDefine(log);
   case IncludeFollowsCode:
      return SortIncludes();
   case IncludeGuardMissing:
      return InsertIncludeGuard(log);
   case IncludeNotSorted:
      return SortIncludes();
   case IncludeDuplicated:
      return EraseCode(log.item_);
   case IncludeAdd:
      return InsertInclude(log);
   case IncludeRemove:
      return EraseCode(log.item_);
   case RemoveOverrideTag:
      return EraseOverrideTag(log);
   case UsingInHeader:
      return ReplaceUsing(log);
   case UsingDuplicated:
      return EraseCode(log.item_);
   case UsingAdd:
      return InsertUsing(log);
   case UsingRemove:
      return EraseCode(log.item_);
   case ForwardAdd:
      return InsertForward(log);
   case ForwardRemove:
      return EraseForward(log);
   case ArgumentUnused:
      return FixInvokers(cli, log);
   case ClassUnused:
      return EraseClass(log);
   case DataUnused:
      return EraseData(cli, log);
   case EnumUnused:
      return EraseCode(log.item_);
   case EnumeratorUnused:
      return EraseCode(log.item_);
   case FriendUnused:
      return EraseCode(log.item_);
   case FunctionUnused:
      return FixFunctions(cli, log);
   case TypedefUnused:
      return EraseCode(log.item_);
   case ForwardUnresolved:
      return EraseForward(log);
   case FriendUnresolved:
      return EraseCode(log.item_);
   case FriendAsForward:
      return InsertForward(log);
   case HidesInheritedName:
      return ReplaceName(log);
   case ClassCouldBeNamespace:
      return ChangeClassToNamespace(log);
   case ClassCouldBeStruct:
      return ChangeClassToStruct(log);
   case StructCouldBeClass:
      return ChangeStructToClass(log);
   case RedundantAccessControl:
      return EraseAccessControl(log);
   case ItemCouldBePrivate:
      return ChangeAccess(log, Cxx::Private);
   case ItemCouldBeProtected:
      return ChangeAccess(log, Cxx::Protected);
   case AnonymousEnum:
      return InsertEnumName(log);
   case DataUninitialized:
      return InsertDataInit(log);
   case DataInitOnly:
      return EraseData(cli, log);
   case DataWriteOnly:
      return EraseData(cli, log);
   case DataCouldBeConst:
      return TagAsConstData(log);
   case DataCouldBeConstPtr:
      return TagAsConstPointer(log);
   case DataNeedNotBeMutable:
      return EraseMutableTag(log);
   case ImplicitPODConstructor:
      return InsertPODCtor(log);
   case ImplicitConstructor:
      return InsertSpecialFunctions(cli, log.item_);
   case ImplicitCopyConstructor:
      return InsertSpecialFunctions(cli, log.item_);
   case ImplicitCopyOperator:
      return InsertSpecialFunctions(cli, log.item_);
   case PublicConstructor:
      return ChangeSpecialFunction(cli, log);
   case NonExplicitConstructor:
      return TagAsExplicit(log);
   case MemberInitMissing:
      return InsertMemberInit(log);
   case MemberInitNotSorted:
      return MoveMemberInit(log);
   case ImplicitDestructor:
      return InsertSpecialFunctions(cli, log.item_);
   case VirtualDestructor:
      return ChangeAccess(log, Cxx::Public);
   case NonVirtualDestructor:
      return ChangeSpecialFunction(cli, log);
   case RuleOf3DtorNoCopyCtor:
      return InsertSpecialFunctions(cli, log.item_);
   case RuleOf3DtorNoCopyOper:
      return InsertSpecialFunctions(cli, log.item_);
   case RuleOf3CopyCtorNoOper:
      return InsertSpecialFunctions(cli, log.item_);
   case RuleOf3CopyOperNoCtor:
      return InsertSpecialFunctions(cli, log.item_);
   case FunctionNotDefined:
      return EraseCode(log.item_);
   case PureVirtualNotDefined:
      return InsertPureVirtual(log);
   case VirtualAndPublic:
      return FixFunctions(cli, log);
   case FunctionNotOverridden:
      return EraseVirtualTag(log);
   case RemoveVirtualTag:
      return EraseVirtualTag(log);
   case OverrideTagMissing:
      return TagAsOverride(log);
   case VoidAsArgument:
      return EraseVoidArgument(log);
   case AnonymousArgument:
      return RenameArgument(cli, log);
   case DefinitionRenamesArgument:
      return RenameArgument(cli, log);
   case OverrideRenamesArgument:
      return RenameArgument(cli, log);
   case VirtualDefaultArgument:
      return FixInvokers(cli, log);
   case ArgumentCouldBeConstRef:
      return FixFunctions(cli, log);
   case ArgumentCouldBeConst:
      return FixFunctions(cli, log);
   case FunctionCouldBeConst:
      return FixFunctions(cli, log);
   case FunctionCouldBeStatic:
      return FixFunctions(cli, log);
   case FunctionCouldBeFree:
      return FixInvokers(cli, log);
   case StaticFunctionViaMember:
      return ChangeOperator(log);
   case UseOfTab:
      return ConvertTabsToBlanks();
   case Indentation:
      return AdjustIndentation(log);
   case TrailingSpace:
      return EraseTrailingBlanks();
   case AdjacentSpaces:
      return EraseAdjacentSpaces(log);
   case AddBlankLine:
      return InsertBlankLine(log);
   case RemoveLine:
      return EraseBlankLine(log);
   case LineLength:
      return InsertLineBreak(log);
   case FunctionNotSorted:
      return MoveFunction(log);
   case HeadingNotStandard:
      return ReplaceHeading(log);
   case IncludeGuardMisnamed:
      return RenameIncludeGuard(log);
   case DebugFtNotInvoked:
      return InsertDebugFtCall(cli, log);
   case DebugFtNameMismatch:
      return ChangeDebugFtName(cli, log);
   case DebugFtNameDuplicated:
      return ChangeDebugFtName(cli, log);
   case DisplayNotOverridden:
      return InsertDisplay(cli, log);
   case PatchNotOverridden:
      return InsertPatch(cli, log);
   case FunctionCouldBeDefaulted:
      return FixFunctions(cli, log);
   case InitCouldUseConstructor:
      return InitByCtorCall(log);
   case CouldBeNoexcept:
      return FixFunctions(cli, log);
   case ShouldNotBeNoexcept:
      return FixFunctions(cli, log);
   case UseOfSlashAsterisk:
      return ReplaceSlashAsterisk(log);
   case RemoveLineBreak:
      return EraseLineBreak(log);
   case CopyCtorConstructsBase:
      return InsertCopyCtorCall(log);
   case FunctionCouldBeMember:
      return FixInvokers(cli, log);
   case ExplicitConstructor:
      return EraseExplicitTag(log);
   case BitwiseOperatorOnBoolean:
      return ChangeOperator(log);
   case DebugFtCanBeLiteral:
      return InlineDebugFtName(log);
   case DataCouldBeFree:
      return ChangeDataToFree(log);
   case ConstructorNotPrivate:
      return ChangeSpecialFunction(cli, log);
   case DestructorNotPrivate:
      return ChangeSpecialFunction(cli, log);
   case RedundantScope:
      return EraseScope(log);
   case OperatorSpacing:
      return AdjustOperator(log);
   case PunctuationSpacing:
      return AdjustPunctuation(log);
   }

   return Report(NotImplemented);
}

//------------------------------------------------------------------------------

word Editor::Format(string& expl)
{
   Debug::ft("Editor.Format");

   EraseTrailingBlanks();
   AdjustVerticalSeparation();
   ConvertTabsToBlanks();
   auto rc = Write();
   expl = GetExpl();
   return rc;
}

//------------------------------------------------------------------------------

size_t Editor::IncludesBegin() const
{
   Debug::ft("Editor.IncludesBegin");

   for(size_t pos = 0; pos != string::npos; pos = NextBegin(pos))
   {
      if(IsDirective(pos, HASH_INCLUDE_STR)) return pos;
   }

   return string::npos;
}

//------------------------------------------------------------------------------

size_t Editor::IncludesEnd() const
{
   Debug::ft("Editor.IncludesEnd");

   for(auto pos = IncludesBegin(); pos != string::npos; pos = NextBegin(pos))
   {
      if(IsDirective(pos, HASH_INCLUDE_STR)) continue;
      if(NoCodeFollows(pos)) continue;

      //  We found something else.  Back up to the last #include and return
      //  the line that follows it.
      //
      pos = PrevBegin(pos);
      while(NoCodeFollows(pos)) pos = PrevBegin(pos);
      return NextBegin(pos);
   }

   return string::npos;
}

//------------------------------------------------------------------------------

size_t Editor::Indent(size_t pos)
{
   Debug::ft("Editor.Indent");

   auto code = GetCode(pos);
   if(code.empty() || (code.front() == CRLF)) return pos;

   auto info = GetLineInfo(pos);
   int depth = 0;

   if(info->depth != SIZE_MAX)
   {
      depth = info->depth;

      //  If the line is executable code, indent it one level more.  This
      //  avoids indenting comments, braces, and access controls, which
      //  are not executable.
      //
      auto type = PosToType(info->begin);
      if(info->cont && LineTypeAttr::Attrs[type].isExecutable) ++depth;
   }

   auto first = code.find_first_not_of(WhitespaceChars);
   if(first == string::npos) return pos;

   auto indent = depth * IndentSize();

   if(first != indent)
   {
      Erase(info->begin, first);
      Insert(info->begin, spaces(indent));
      Changed();
   }

   return pos;
}

//------------------------------------------------------------------------------

word Editor::InitByCtorCall(const CodeWarning& log)
{
   Debug::ft("Editor.InitByCtorCall");

   //  Change ["const"] <type> <name> = <class> "(" [<args>] ");"
   //      to ["const"] <class> <name> "(" [<args>] ");"
   //
   //  Start by erasing ["const"] <type>, leaving a space before <name>.
   //
   auto begin = CurrBegin(log.Pos());
   if(begin == string::npos) return NotFound("Initialization statement");
   auto first = LineFindFirst(begin);
   if(first == string::npos) return NotFound("Start of code");
   auto name = FindWord(first, log.item_->Name());
   if(name == string::npos) return NotFound("Variable name");
   Erase(first, name - first - 1);

   //  Erase <class>.
   //
   auto eq = FindFirstOf(first, "=");
   if(eq == string::npos) return NotFound("Assignment operator");
   auto cbegin = FindNonBlank(eq + 1);
   if(cbegin == string::npos) return NotFound("Start of class name");
   auto lpar = FindFirstOf(eq, "(");
   if(lpar == string::npos) return NotFound("Left parenthesis");
   auto cend = RfindNonBlank(lpar - 1);
   if(cend == string::npos) return NotFound("End of class name");
   auto cname = source_.substr(cbegin, cend - cbegin + 1);
   Erase(cbegin, cend - cbegin + 1);

   //  Paste <class> before <name> and make it const if necessary.
   //
   Paste(first, cname, cbegin);
   if(log.item_->IsConst()) first = Insert(first, "const ");

   //  Remove the '=' and the spaces around it.
   //
   eq = FindFirstOf(first, "=");
   if(eq == string::npos) return NotFound("Assignment operator");
   auto left = RfindNonBlank(eq - 1);
   auto right = FindNonBlank(eq + 1);
   Erase(left + 1, right - left - 1);

   //  If there are no arguments, remove the parentheses.
   //
   lpar = FindFirstOf(left, "(");
   if(lpar == string::npos) return NotFound("Left parenthesis");
   auto rpar = FindClosing('(', ')', lpar + 1);
   if(rpar == string::npos) return NotFound("Right parenthesis");
   if(source_.find_first_not_of(WhitespaceChars, lpar + 1) == rpar)
   {
      Erase(lpar, rpar - lpar + 1);
   }

   //  If the code spanned two lines, it may be possible to remove the
   //  line break.
   //
   auto semi = FindFirstOf(lpar - 1, ";");
   if(!OnSameLine(begin, semi)) EraseLineBreak(begin);
   return Changed(begin);
}

//------------------------------------------------------------------------------

word Editor::InlineDebugFtName(const CodeWarning& log)
{
   Debug::ft("Editor.InlineDebugFtName");

   //  Find the fn_name data, in-line its string literal in the Debug::ft
   //  call, and then erase it.
   //
   string fname;
   auto data = static_cast< const Data* >(log.item_);
   if(data == nullptr) return NotFound("fn_name declaration");
   if(!data->GetStrValue(fname)) return NotFound("fn_name definition");

   auto dpos = data->GetPos();
   if(dpos == string::npos) return NotFound("fn_name in source");
   auto split = (LineFind(dpos, ";") == string::npos);
   auto next = EraseLine(dpos);
   if(split) EraseLine(next);

   auto literal = QUOTE + fname + QUOTE;
   auto begin = CurrBegin(log.Pos());
   if(begin == string::npos) return NotFound("Position of Debug::ft");
   auto lpar = source_.find('(', begin);
   if(lpar == string::npos) return NotFound("Left parenthesis");
   auto rpar = source_.find(')', lpar);
   if(rpar == string::npos) return NotFound("Right parenthesis");
   Replace(lpar + 1, rpar - lpar - 1, literal);
   return Changed(lpar);
}

//------------------------------------------------------------------------------

size_t Editor::Insert(size_t pos, const string& code)
{
   Debug::ft("Editor.Insert");

   source_.insert(pos, code);
   Update();
   file_->UpdatePos(Inserted, pos, code.size());
   UpdateWarnings(Inserted, pos, code.size());
   Changed();
   return pos;
}

//------------------------------------------------------------------------------

void Editor::InsertAfterFuncDefn(const FuncDefnAttrs& attrs)
{
   Debug::ft("Editor.InsertAfterFuncDefn");

   if(attrs.blank == BlankAfter)
   {
      InsertLine(attrs.pos, EMPTY_STR);

      if(attrs.rule)
      {
         InsertRule(attrs.pos, '-');
         InsertLine(attrs.pos, EMPTY_STR);
      }
   }
}

//------------------------------------------------------------------------------

void Editor::InsertAfterItemDecl(const ItemDeclAttrs& attrs)
{
   Debug::ft("Editor.InsertAfterItemDecl");

   if(attrs.nextctrl != Cxx::Access_N)
   {
      std::ostringstream access;
      access << attrs.nextctrl << ':';
      InsertLine(attrs.pos, access.str());
   }
   else if(attrs.blank == BlankAfter)
   {
      InsertLine(attrs.pos, EMPTY_STR);
   }
}

//------------------------------------------------------------------------------

word Editor::InsertArgument(const Function* func, word offset)
{
   Debug::ft("Editor.InsertArgument");

   //  Change all invocations of FUNC so that any which use the default
   //  value for this argument pass the default value explicitly.
   //
   return Unimplemented();
}

//------------------------------------------------------------------------------

void Editor::InsertBeforeFuncDefn(const FuncDefnAttrs& attrs)
{
   Debug::ft("Editor.InsertBeforeFuncDefn");

   if(attrs.blank == BlankBefore)
   {
      InsertLine(attrs.pos, EMPTY_STR);

      if(attrs.rule)
      {
         InsertRule(attrs.pos, '-');
         InsertLine(attrs.pos, EMPTY_STR);
      }
   }
}

//------------------------------------------------------------------------------

void Editor::InsertBeforeItemDecl
   (const ItemDeclAttrs& attrs, const string& comment)
{
   Debug::ft("Editor.InsertBeforeItemDecl");

   if(attrs.comment)
   {
      InsertLine(attrs.pos, strComment(EMPTY_STR, attrs.indent));
      Insert(attrs.pos, strComment(comment, attrs.indent));
   }

   if(attrs.thisctrl)
   {
      std::ostringstream access;
      access << attrs.access << ':';
      InsertLine(attrs.pos, access.str());
   }
   else if(attrs.blank == BlankBefore)
   {
      InsertLine(attrs.pos, EMPTY_STR);
   }
}

//------------------------------------------------------------------------------

word Editor::InsertBlankLine(const CodeWarning& log)
{
   Debug::ft("Editor.InsertBlankLine");

   auto begin = CurrBegin(log.Pos());
   if(begin == string::npos) return NotFound("Position for blank line");
   InsertLine(begin, EMPTY_STR);
   return EditSucceeded;
}

//------------------------------------------------------------------------------

word Editor::InsertCopyCtorCall(const CodeWarning& log)
{
   Debug::ft("Editor.InsertCopyCtorCall");

   //  Have this copy constructor invoke its base class copy constructor.
   //
   return Unimplemented();
}

//------------------------------------------------------------------------------

word Editor::InsertDataInit(const CodeWarning& log)
{
   Debug::ft("Editor.InsertDataInit");

   //  Initialize this data item to its default value.
   //
   return Unimplemented();
}

//------------------------------------------------------------------------------

word Editor::InsertDebugFtCall(CliThread& cli, const CodeWarning& log)
{
   Debug::ft("Editor.InsertDebugFtCall");

   auto name = log.item_->GetPos();
   if(name == string::npos) return NotFound("Function name");

   size_t begin, left, right;
   auto func = static_cast< const Function* >(log.item_);
   func->GetRange(begin, left, right);
   if(left == string::npos) return NotFound("Function definition");

   //  Get the start of the name for an fn_name declaration and the inline
   //  string literal for the Debug::ft invocation.  Set EXTRA if anything
   //  follows the left brace on the same line.
   //
   string flit, fvar;
   DebugFtNames(func, flit, fvar);
   auto extra = (LineFindNext(left + 1) != string::npos);

   //  There are two possibilities:
   //  o An fn_name is already defined (e.g. for invoking Debug::SwLog), in
   //    which case it will be located before the end of the function.  Its
   //    name will start with FVAR but could be longer if the function is
   //    overloaded, so extract everything to the closing ')' in the call.
   //  o An fn_name is not already defined, in which case the string literal
   //    (FLIT) can be used.
   //
   string arg;

   for(auto pos = left; (pos < right) && arg.empty(); pos = NextBegin(pos))
   {
      auto start = LineFind(pos, fvar);

      if(start != string::npos)
      {
         auto end = source_.find_first_not_of(ValidNextChars, start);
         if(end == string::npos) return NotFound("End of fn_name");
         arg = source_.substr(start, end - start);
      }
   }

   if(arg.empty())
   {
      //  No fn_name was defined, so use FLIT.  If another function already
      //  uses that name, prompt the user for a suffix.
      //
      if(!EnsureUniqueDebugFtName(cli, flit, arg))
         return Report(FixSkipped);
   }

   //  Create the call to Debug::ft at the top of the function, after its
   //  left brace:
   //  o If something followed the left brace, push it down.
   //  o Insert a blank line after the call to Debug::ft.
   //  o Insert the call to Debug::ft.
   //  o If the left brace wasn't at the start of its line, push it down.
   //
   if(extra) InsertLineBreak(left + 1);
   auto below = NextBegin(left);
   InsertLine(below, EMPTY_STR);
   auto call = DebugFtCode(arg);
   InsertLine(below, call);
   if(!IsFirstNonBlank(left)) InsertLineBreak(left);
   return Changed(below);
}

//------------------------------------------------------------------------------

word Editor::InsertDisplay(CliThread& cli, const CodeWarning& log)
{
   Debug::ft("Editor.InsertDisplay");

   //  Declare an override and put "To be implemented" in the definition, with
   //  an invocation of the base class's Display function.  A more ambitious
   //  implementation would display members or invoke *their* Display functions.
   //
   return Unimplemented();
}

//------------------------------------------------------------------------------

word Editor::InsertEnumName(const CodeWarning& log)
{
   Debug::ft("Editor.InsertEnumName");

   //  Prompt for the enum's name and insert it.
   //
   return Unimplemented();
}

//------------------------------------------------------------------------------

word Editor::InsertForward(const CodeWarning& log)
{
   Debug::ft("Editor.InsertForward(log)");

   //  LOGS provides the forward's namespace and any template parameters.
   //
   string forward = spaces(IndentSize()) + log.info_ + ';';
   auto srPos = forward.find(SCOPE_STR);
   if(srPos == string::npos) return NotFound("Forward's namespace.");

   //  Extract the namespace.
   //
   auto areaPos = forward.find("class ");
   if(areaPos == string::npos) areaPos = forward.find("struct ");
   if(areaPos == string::npos) areaPos = forward.find("union ");
   if(areaPos == string::npos) return NotFound("Forward's area type");

   //  Set NSPACE to "namespace <ns>", where <ns> is the symbol's namespace.
   //  Erase <ns> from FORWARD and then decide where to insert the forward
   //  declaration.
   //
   string nspace = NAMESPACE_STR;
   auto nsPos = forward.find(SPACE, areaPos);
   auto nsName = forward.substr(nsPos, srPos - nsPos);
   nspace += nsName;
   forward.erase(nsPos + 1, srPos - nsPos + 1);
   auto begin = CodeBegin();

   for(auto pos = PrologEnd(); pos != string::npos; pos = NextBegin(pos))
   {
      if(CodeMatches(pos, NAMESPACE_STR))
      {
         //  If this namespace matches NSPACE, add the declaration to it.
         //  If this namespace's name is alphabetically after NSPACE, add
         //  the declaration before it, along with its namespace.
         //
         auto comp = source_.compare(pos, nspace.size(), nspace);
         if(comp == 0) return InsertForward(pos, forward);
         if(comp > 0) return InsertNamespaceForward(pos, nspace, forward);
      }
      else if(CodeMatches(pos, USING_STR) || (pos == begin))
      {
         //  We have now passed any existing forward declarations, so add
         //  the new declaration here, along with its namespace.
         //
         return InsertNamespaceForward(pos, nspace, forward);
      }
   }

   return Report("Failed to insert forward declaration.");
}

//------------------------------------------------------------------------------

word Editor::InsertForward(size_t pos, const string& forward)
{
   Debug::ft("Editor.InsertForward(iter)");

   //  POS references a namespace that matches the one for a new forward
   //  declaration.  Insert the new declaration alphabetically within the
   //  declarations that already appear in this namespace.  Note that it
   //  may already have been inserted while fixing another warning.
   //
   for(pos = NextBegin(pos); pos != string::npos; pos = NextBegin(pos))
   {
      auto first = LineFindFirst(pos);
      if(source_[first] == '{') continue;

      auto comp = source_.compare(pos, forward.size(), forward);
      if(comp == 0) return Report("Previously inserted.");

      if((comp > 0) || (source_[first] == '}'))
      {
         InsertLine(pos, forward);
         return Changed(pos);
      }
   }

   return Report("Failed to insert forward declaration.");
}

//------------------------------------------------------------------------------

word Editor::InsertInclude(const CodeWarning& log)
{
   Debug::ft("Editor.InsertInclude(log)");

   //  Create the new #include directive and add it to the list.
   //  Its file name appears in log.info_.
   //
   string include = HASH_INCLUDE_STR;
   include.push_back(SPACE);
   include += log.info_;
   return InsertInclude(include);
}

//------------------------------------------------------------------------------

word Editor::InsertInclude(string& include)
{
   Debug::ft("Editor.InsertInclude(string)");

   //  Start by mangling the new #include and all existing ones, which
   //  makes it easier to insert the new one in the correct position.
   //
   auto rc = MangleInclude(include);
   if(rc != EditContinue) return rc;
   MangleIncludes();

   //  Insert the new #include in its sort order.
   //
   auto end = IncludesEnd();

   for(auto pos = IncludesBegin(); pos != end; pos = NextBegin(pos))
   {
      if(NoCodeFollows(pos)) continue;

      if(!IncludesAreSorted(GetCode(pos), include))
      {
         InsertLine(pos, include);
         return Changed(pos);
      }
   }

   //  Add the new #include to the end of the list.  If there is no
   //  blank line at the end of the list, add one.
   //
   auto pos = end;

   if(!IsBlankLine(end))
   {
      pos = InsertLine(end, EMPTY_STR);
   }

   InsertLine(pos, include);
   return Changed(pos);
}

//------------------------------------------------------------------------------

word Editor::InsertIncludeGuard(const CodeWarning& log)
{
   Debug::ft("Editor.InsertIncludeGuard");

   //  Insert the guard before the first #include.  If there isn't one,
   //  insert it before the first line of code.  If there isn't any code,
   //  insert it at the end of the file.
   //
   auto pos = IncludesBegin();
   if(pos == string::npos) pos = PrologEnd();
   string guardName = log.File()->MakeGuardName();
   string code = "#define " + guardName;
   pos = InsertLine(pos, EMPTY_STR);
   pos = InsertLine(pos, code);
   code = "#ifndef " + guardName;
   InsertLine(pos, code);
   code = string(HASH_ENDIF_STR) + CRLF_STR;
   Insert(source_.size(), code);
   return Changed(pos);
}

//------------------------------------------------------------------------------

size_t Editor::InsertLine(size_t pos, const string& code)
{
   Debug::ft("Editor.InsertLine");

   if(pos >= source_.size()) return string::npos;
   auto copy = code;
   if(copy.empty() || (copy.back() != CRLF)) copy.push_back(CRLF);
   return Insert(pos, copy);
}

//------------------------------------------------------------------------------

size_t Editor::InsertLineBreak(size_t pos)
{
   Debug::ft("Editor.InsertLineBreak(pos)");

   auto begin = CurrBegin(pos);
   if(begin == string::npos) return string::npos;
   auto end = CurrEnd(pos);
   if((pos == begin) || (pos == end)) return string::npos;
   if(IsBlankLine(pos)) return string::npos;
   Insert(pos, CRLF_STR);
   Indent(NextBegin(pos));
   return pos;
}

//------------------------------------------------------------------------------

word Editor::InsertLineBreak(const CodeWarning& log)
{
   Debug::ft("Editor.InsertLineBreak(log)");

   //  Consider parentheses, lexical level, binary operators...
   //
   auto begin = CurrBegin(log.Pos());
   if(begin == string::npos) return NotFound("Position for line break");

   return Unimplemented();
}

//------------------------------------------------------------------------------

word Editor::InsertMemberInit(const CodeWarning& log)
{
   Debug::ft("Editor.InsertMemberInit");

   //  Initialize the member to its default value.
   //
   return Unimplemented();
}

//------------------------------------------------------------------------------

word Editor::InsertNamespaceForward
   (size_t pos, const string& nspace, const string& forward)
{
   Debug::ft("Editor.InsertNamespaceForward");

   //  Insert a new forward declaration, along with an enclosing namespace,
   //  at POS.  Offset it with blank lines.
   //
   InsertLine(pos, EMPTY_STR);
   InsertLine(pos, "}");
   InsertLine(pos, forward);
   InsertLine(pos, "{");
   InsertLine(pos, nspace);
   InsertLine(pos, EMPTY_STR);
   pos = Find(pos, forward);
   return Changed(pos);
}

//------------------------------------------------------------------------------

fixed_string PatchComment = "Overridden for patching.";
fixed_string PatchReturn = "void";
fixed_string PatchSignature = "Patch(sel_t selector, void* arguments)";
fixed_string PatchInvocation = "Patch(selector, arguments)";

word Editor::InsertPatch(CliThread& cli, const CodeWarning& log)
{
   Debug::ft("Editor.InsertPatch");

   //  Extract the name of the function to insert.  (It will be "Patch",
   //  but this code might eventually be generalized for other functions.)
   //
   auto cls = static_cast< const Class* >(log.item_);
   auto name = log.GetNewFuncName();
   if(name.empty()) return Report("Log did not specify function name.");

   //  Find out where the function's declaration should be inserted and which
   //  file should contain its definition.  The definition is in another file,
   //  so access that file's editor and find out where the function should be
   //  defined in that file.
   //
   ItemDeclAttrs decl(Cxx::Function, Cxx::Public);
   decl.over = true;
   auto rc = FindItemDeclLoc(cls, name, decl);
   if(rc != EditContinue) return Report(UnspecifiedFailure, rc);

   auto file = FindFuncDefnFile(cli, cls, name);
   if(file == nullptr) return NotFound("File for definition");

   auto& editor = file->GetEditor();

   FuncDefnAttrs defn(FuncOther);
   rc = editor.FindFuncDefnLoc(file, cls, name, defn);
   if(rc != EditContinue) return rc;

   //  Insert the function's declaration and definition.
   //
   InsertPatchDecl(decl);
   editor.InsertPatchDefn(cls, defn);
   auto pos = Find(defn.pos, PatchSignature);
   return Changed(pos);
}

//------------------------------------------------------------------------------

void Editor::InsertPatchDecl(const ItemDeclAttrs& attrs)
{
   Debug::ft("Editor.InsertPatchDecl");

   InsertAfterItemDecl(attrs);

   string code = PatchReturn;
   code.push_back(SPACE);
   code.append(PatchSignature);
   code.push_back(SPACE);
   code.append(OVERRIDE_STR);
   code.push_back(';');
   InsertLine(attrs.pos, strCode(code, 1));

   InsertBeforeItemDecl(attrs, PatchComment);
}

//------------------------------------------------------------------------------

void Editor::InsertPatchDefn(const Class* cls, const FuncDefnAttrs& attrs)
{
   Debug::ft("Editor.InsertPatchDefn");

   InsertAfterFuncDefn(attrs);

   InsertLine(attrs.pos, "}");

   auto base = cls->BaseClass();
   auto code = base->Name();
   code.append(SCOPE_STR);
   code.append(PatchInvocation);
   code.push_back(';');
   InsertLine(attrs.pos, strCode(code, 1));

   InsertLine(attrs.pos, "{");

   code = PatchReturn;
   code.push_back(SPACE);
   code.append(cls->Name());
   code.append(SCOPE_STR);
   code.append(PatchSignature);
   InsertLine(attrs.pos, code);

   InsertBeforeFuncDefn(attrs);
}

//------------------------------------------------------------------------------

word Editor::InsertPODCtor(const CodeWarning& log)
{
   Debug::ft("Editor.InsertPODCtor");

   //  Declare and define a constructor that initializes POD members to
   //  their default values.
   //
   return Unimplemented();
}

//------------------------------------------------------------------------------

size_t Editor::InsertPrefix(size_t pos, const string& prefix)
{
   Debug::ft("Editor.InsertPrefix");

   auto first = LineFindFirst(pos);
   if(first == string::npos) return string::npos;

   if(pos + prefix.size() <= first)
   {
      Replace(pos, prefix.size(), prefix);
   }
   else
   {
      Insert(pos, prefix);
   }

   Changed();
   return pos;
}

//------------------------------------------------------------------------------

word Editor::InsertPureVirtual(const CodeWarning& log)
{
   Debug::ft("Editor.InsertPureVirtual");

   //  Insert a definition that invokes Debug::SwLog with strOver(this).
   //
   return Unimplemented();
}

//------------------------------------------------------------------------------

size_t Editor::InsertRule(size_t pos, char c)
{
   Debug::ft("Editor.InsertRule");

   string rule = COMMENT_STR;
   rule.append(LineLengthMax() - 2, c);
   InsertLine(pos, rule);
   return pos;
}

//------------------------------------------------------------------------------

word Editor::InsertSpecialFuncDecl
   (CliThread& cli, const Class* cls, FunctionRole role)
{
   Debug::ft("Editor.InsertSpecialFuncDecl");

   ItemDeclAttrs attrs(Cxx::Function, Cxx::Public, role);
   auto rc = FindSpecialFuncDeclLoc(cli, cls, attrs);
   if(rc != EditContinue) return rc;

   auto code = spaces(attrs.indent);
   auto className = cls->Name();

   string defn;

   if(!attrs.shell)
   {
      defn += " = ";
      defn += (attrs.deleted ? DELETE_STR : DEFAULT_STR);
   }

   switch(attrs.role)
   {
   case PureCtor:
      code += className + "()";
      break;

   case CopyCtor:
      code += className;
      code += "(const " + className + "& that)";
      break;

   case CopyOper:
      code += className + "& " + "operator=";
      code += "(const " + className + "& that)";
      break;

   case PureDtor:
      if(attrs.virt) code += string(VIRTUAL_STR) + SPACE;
      code += '~' + className + "()";
      break;

   default:
      return Report("Unexpected special member function.");
   }

   code += defn;
   code.push_back(';');
   InsertAfterItemDecl(attrs);
   InsertLine(attrs.pos, code);
   code.pop_back();

   auto comment = CreateSpecialFunctionComment(attrs);
   InsertBeforeItemDecl(attrs, comment);

   if(attrs.shell)
   {
      auto file = FindFuncDefnFile(cli, cls, code);
      if(file == nullptr) return NotFound("File for function definition");

      auto& editor = file->GetEditor();

      FuncDefnAttrs defn(attrs.role);
      rc = editor.FindFuncDefnLoc(file, cls, code, defn);
      if(rc != EditContinue) return rc;

      //  Insert the function's declaration and definition.
      //
      editor.InsertSpecialFuncDefn(cls, defn);
   }

   auto pos = source_.find(code, attrs.pos);
   return Changed(pos);
}

//------------------------------------------------------------------------------

void Editor::InsertSpecialFuncDefn
   (const Class* cls, const FuncDefnAttrs& attrs)
{
   Debug::ft("Editor.InsertSpecialFuncDefn");

   InsertAfterFuncDefn(attrs);
   InsertLine(attrs.pos, "}");

   auto code = spaces(IndentSize());
   code += COMMENT_STR;
   code += "* To be implemented.";
   InsertLine(attrs.pos, code);
   InsertLine(attrs.pos, "{");

   auto className = cls->Name();
   code.clear();

   if(attrs.role == CopyOper)
   {
      code = className;
      code += "& ";
   }

   code += className;
   code += SCOPE_STR;

   switch(attrs.role)
   {
   case PureCtor:
      code += className + "()";
      break;

   case CopyCtor:
      code += className;
      code += "(const " + className + "& that)";
      break;

   case CopyOper:
      code += className + "& " + "operator=";
      code += "(const " + className + "& that)";
      break;

   case PureDtor:
      code += '~' + className + "()";
      break;
   }

   InsertLine(attrs.pos, code);
   InsertBeforeFuncDefn(attrs);
}

//------------------------------------------------------------------------------

const size_t MaxRoleWarning = 12;

struct RoleWarning
{
   const FunctionRole role;  // type of special member function to add
   const Warning log;        // type of log that calls for its addition
};

//  Logs associated with adding a special member function.
//
const RoleWarning SpecialMemberFunctionLogs[MaxRoleWarning] =
{
   { PureCtor, ImplicitConstructor },
   { PureCtor, PublicConstructor },
   { PureCtor, ConstructorNotPrivate },
   { PureDtor, ImplicitDestructor },
   { PureDtor, NonVirtualDestructor },
   { PureDtor, DestructorNotPrivate },
   { CopyCtor, ImplicitCopyConstructor },
   { CopyCtor, RuleOf3DtorNoCopyCtor },
   { CopyCtor, RuleOf3CopyOperNoCtor },
   { CopyOper, ImplicitCopyOperator },
   { CopyOper, RuleOf3DtorNoCopyOper },
   { CopyOper, RuleOf3CopyCtorNoOper }
};

//  Maps a warning to the type of function associated with it.
//
FunctionRole WarningToRole(Warning log)
{
   for(size_t i = 0; i < MaxRoleWarning; ++i)
   {
      if(SpecialMemberFunctionLogs[i].log == log)
         return SpecialMemberFunctionLogs[i].role;
   }

   return FuncRole_N;
}

//------------------------------------------------------------------------------

word Editor::InsertSpecialFunctions(CliThread& cli, const CxxToken* item)
{
   Debug::ft("Editor.InsertSpecialFunctions");

   //  This coordinates adding all missing special member functions
   //  that can be defined as defaulted or deleted.
   //
   auto cls = static_cast< const Class* >(item);
   std::vector< CodeWarning* > logs;
   std::set< FunctionRole > roles;

   for(size_t i = 0; i < MaxRoleWarning; ++i)
   {
      auto log = FindLog(SpecialMemberFunctionLogs[i].log, item, 0);

      if((log != nullptr) && (log->status_ == NotFixed))
      {
         logs.push_back(log);
         roles.insert(SpecialMemberFunctionLogs[i].role);
      }
   }

   //  Handle the Rule of 3: if adding the destructor, copy constructor,
   //  or copy operator, add any of the others that are missing.
   //
   if((roles.find(PureDtor) != roles.cend()) ||
      (roles.find(CopyCtor) != roles.cend()) ||
      (roles.find(CopyOper) != roles.cend()))
   {
      auto dtor = cls->FindFuncByRole(PureDtor, false);
      auto copy = cls->FindFuncByRole(CopyCtor, false);
      auto oper = cls->FindFuncByRole(CopyOper, false);
      if(dtor == nullptr) roles.insert(PureDtor);
      if(copy == nullptr) roles.insert(CopyCtor);
      if(oper == nullptr) roles.insert(CopyOper);
   }

   auto prompt = (roles.size() > 1);
   word rc = EditCompleted;

   if(prompt)
   {
      *cli.obuf << "This will also prompt to add related functions..." << CRLF;
      cli.Flush();
   }

   //  Add each function in ROLES and update the warnings associated with it.
   //
   for(int r = PureCtor; r < FuncOther; ++r)
   {
      auto role = static_cast< FunctionRole >(r);

      if(roles.find(role) != roles.cend())
      {
         roles.erase(role);

         auto add = true;

         if(prompt)
         {
            std::ostringstream stream;
            stream << "Add the " << role << '?';
            add = cli.BoolPrompt(stream.str());
         }

         if(add)
         {
            rc = InsertSpecialFuncDecl(cli, cls, role);
            ReportFix(cli, nullptr, rc);
         }

         if(rc == EditSucceeded)
         {
            for(size_t i = 0; i < logs.size(); ++i)
            {
               if(WarningToRole(logs[i]->warning_) == role)
                  logs[i]->status_ = Pending;
            }
         }
      }
   }

   return EditCompleted;
}

//------------------------------------------------------------------------------

word Editor::InsertUsing(const CodeWarning& log)
{
   Debug::ft("Editor.InsertUsing");

   //  Create the new using statement and decide where to insert it.
   //
   string statement = USING_STR;
   statement.push_back(SPACE);
   statement += log.info_;
   statement.push_back(';');

   auto stop = CodeBegin();
   auto usings = false;

   for(auto pos = PrologEnd(); pos != string::npos; pos = NextBegin(pos))
   {
      if(CodeMatches(pos, USING_STR))
      {
         //  If this using statement is alphabetically after STATEMENT,
         //  add the new statement before it.
         //
         usings = true;

         if(source_.compare(pos, statement.size(), statement) > 0)
         {
            InsertLine(pos, statement);
            return Changed(pos);
         }
      }
      else if((usings && IsBlankLine(pos)) || (pos >= stop))
      {
         //  We have now passed any existing using statements, so add
         //  the new statement here.  If it is the first one, set it
         //  off with blank lines.
         //
         if(!usings) InsertLine(pos, EMPTY_STR);
         InsertLine(pos, statement);
         if(!usings) InsertLine(pos, EMPTY_STR);
         return Changed(pos + 1);
      }
   }

   return Report("Failed to insert using statement.");
}

//------------------------------------------------------------------------------

size_t Editor::IntroStart(size_t pos, bool funcName) const
{
   Debug::ft("Editor.IntroStart");

   auto start = pos;
   auto found = false;

   for(auto curr = PrevBegin(pos); curr != string::npos; curr = PrevBegin(curr))
   {
      auto type = PosToType(curr);

      switch(type)
      {
      case EmptyComment:
      case TextComment:
      case SlashAsteriskComment:
         //
         //  These are attached to the code that follows, so include them.
         //
         start = curr;
         break;

      case BlankLine:
         //
         //  This can be included if it precedes or follows an fn_name.
         //
         if(!funcName) return start;
         if(found) start = curr;
         break;

      case FunctionName:
         if(!funcName) return start;
         found = true;
         start = curr;

      default:
         return start;
      }
   }

   return pos;
}

//------------------------------------------------------------------------------

bool Editor::IsDirective(size_t pos, fixed_string hash) const
{
   return (source_.compare(pos, strlen(hash), hash) == 0);
}

//------------------------------------------------------------------------------

size_t Editor::LineAfterItem(const CxxToken* item) const
{
   Debug::ft("Editor.LineAfterItem");

   size_t begin, left, end;
   if(!item->GetRange(begin, left, end)) return string::npos;
   return NextBegin(end);
}

//------------------------------------------------------------------------------

word Editor::MangleInclude(string& include) const
{
   Debug::ft("Editor.MangleInclude");

   //  INCLUDE is enclosed in angle brackets or quotes.  To simplify
   //  sorting, the following substitutions are made:
   //  o group 1: enclosed in [ ]
   //  o group 2: enclosed in ' '
   //  o group 3: enclosed in ( )
   //  o group 4: enclosed in ` `
   //  This is fixed when the #include is written out.
   //
   if(include.find(HASH_INCLUDE_STR) != 0)
      return Report("#include not at front of directive.");
   auto first = include.find_first_of(FrontChars);
   if(first == string::npos)
      return Report("Failed to extract file name from #include.");
   auto last = include.find_first_of(BackChars, first + 1);
   if(last == string::npos)
      return Report("Failed to extract file name from #include.");
   auto name = include.substr(first + 1, last - first - 1);
   auto group = file_->CalcGroup(name);
   if(group == 0) return Report("#include specified unknown file.");
   include[first] = FrontChars[group - 1];
   include[last] = BackChars[group - 1];
   return EditContinue;
}

//------------------------------------------------------------------------------

void Editor::MangleIncludes()
{
   Debug::ft("Editor.MangleIncludes");

   for(size_t pos = 0; pos != string::npos; pos = NextBegin(pos))
   {
      if(IsDirective(pos, HASH_INCLUDE_STR))
      {
         auto incl = GetCode(pos);
         MangleInclude(incl);
         source_.erase(pos, incl.size());
         source_.insert(pos, incl);
      }
   }
}

//------------------------------------------------------------------------------

word Editor::MoveDefine(const CodeWarning& log)
{
   Debug::ft("Editor.MoveDefine");

   //  Move this #define directly after the #include directives.
   //
   return Unimplemented();
}

//------------------------------------------------------------------------------

word Editor::MoveFunction(const CodeWarning& log)
{
   Debug::ft("Editor.MoveFunction");

   //  Move the function's definition to the correct location.
   //
   return Unimplemented();
}

//------------------------------------------------------------------------------

word Editor::MoveMemberInit(const CodeWarning& log)
{
   Debug::ft("Editor.MoveMemberInit");

   //  Move the member to the correct location in the initialization list.
   //
   return Unimplemented();
}

//------------------------------------------------------------------------------

fn_name Editor_Paste = "Editor.Paste";

size_t Editor::Paste(size_t pos, const std::string& code, size_t from)
{
   Debug::ft(Editor_Paste);

   if(from != lastCut_)
   {
      Debug::SwLog(Editor_Paste, "Illegal Paste operation: " + code, from);
      return string::npos;
   }

   source_.insert(pos, code);
   lastCut_ = string::npos;
   Update();
   file_->UpdatePos(Pasted, pos, code.size(), from);
   UpdateWarnings(Pasted, pos, code.size(), from);
   Changed();
   return pos;
}

//------------------------------------------------------------------------------

size_t Editor::PrologEnd() const
{
   Debug::ft("Editor.PrologEnd");

   for(size_t pos = 0; pos != string::npos; pos = NextBegin(pos))
   {
      if(LineTypeAttr::Attrs[PosToType(pos)].isCode) return pos;
   }

   return string::npos;
}

//------------------------------------------------------------------------------

void Editor::QualifyReferent(const CxxToken* item, const CxxToken* ref)
{
   Debug::ft("Editor.QualifyReferent");

   //  Within ITEM, prefix NS wherever SYMBOL appears as an identifier.
   //
   const Namespace* ns = ref->GetSpace();
   auto symbol = &ref->Name();

   switch(ref->Type())
   {
   case Cxx::Namespace:
      ns = static_cast< const Namespace* >(ref)->OuterSpace();
      break;
   case Cxx::Class:
      if(ref->IsInTemplateInstance())
      {
         auto tmplt = ref->GetTemplate();
         ns = tmplt->GetSpace();
         symbol = &tmplt->Name();
      }
   }

   auto qual = ns->ScopedName(false) + SCOPE_STR;
   size_t pos, left, end;
   if(!item->GetRange(pos, left, end)) return;
   Reposition(pos);
   string name;

   while(FindIdentifier(name, false) && (Curr() <= end))
   {
      if(name == *symbol)
      {
         //  Qualify NAME with NS if it is not already qualified.
         //
         pos = Curr();

         if(source_.rfind(SCOPE_STR, pos) != (pos - strlen(SCOPE_STR)))
         {
            Insert(pos, qual);
            Changed();
            Advance(qual.size());
         }
      }

      //  Look for the next name after the current one.
      //
      Advance(name.size());
   }
}

//------------------------------------------------------------------------------

void Editor::QualifyUsings(CxxToken* item)
{
   Debug::ft("Editor.QualifyUsings");

   auto refs = FindUsingReferents(item);

   for(auto r = refs.cbegin(); r != refs.cend(); ++r)
   {
      QualifyReferent(item, *r);
   }
}

//------------------------------------------------------------------------------

word Editor::RenameArgument(CliThread& cli, const CodeWarning& log)
{
   Debug::ft("Editor.RenameArgument");

   //  This handles the following argument warnings:
   //  o AnonymousArgument: unnamed argument
   //  o DefinitionRenamesArgument: definition's name differs from declaration's
   //  o OverrideRenamesArgument: override's name differs from root's
   //
   auto func = static_cast< const Function* >(log.item_);
   const Function* decl = func->GetDecl();
   const Function* defn = func->GetDefn();
   const Function* root = (func->IsOverride() ? func->FindRootFunc() : nullptr);
   if(decl == nullptr) return NotFound("Function's declaration");

   //  Use the argument name from the root (if any), else the declaration,
   //  else the definition.
   //
   auto index = decl->LogOffsetToArgIndex(log.offset_);
   string argName;
   auto declName = decl->GetArgs().at(index)->Name();
   string defnName;
   if(defn != nullptr) defnName = defn->GetArgs().at(index)->Name();
   if(root != nullptr) argName = root->GetArgs().at(index)->Name();
   if(argName.empty()) argName = declName;
   if(argName.empty()) argName = defnName;
   if(argName.empty()) return NotFound("Candidate argument name");

   //  The declaration and definition are logged separately, so fix only
   //  the one that has a problem.
   //
   switch(log.warning_)
   {
   case AnonymousArgument:
   {
      auto type = func->GetArgs().at(index)->GetTypeSpec()->GetPos();
      type = FindFirstOf(type, ",)");
      if(type == string::npos) return NotFound("End of argument");
      argName.insert(0, 1, SPACE);
      Insert(type, argName);
      return Changed(type);
   }

   case DefinitionRenamesArgument:
      //
      //  See which name the user wants to use.
      //
      if(!declName.empty() && !defnName.empty())
      {
         argName = ChooseArgumentName(cli, declName, defnName);
         if(argName == defnName) func = decl;
      }
   };

   size_t begin, left, end;
   if(!func->GetRange(begin, left, end)) return NotFound("Function");
   if(func == decl) defnName = declName;
   auto& editor = func->GetFile()->GetEditor();

   for(auto pos = editor.FindWord(begin, defnName); pos < end;
      pos = editor.FindWord(pos + 1, defnName))
   {
      editor.Replace(pos, defnName.size(), argName);
      end = end + argName.size() - defnName.size();
      editor.Changed();
   }

   return editor.Changed(begin);
}

//------------------------------------------------------------------------------

word Editor::RenameIncludeGuard(const CodeWarning& log)
{
   Debug::ft("Editor.RenameIncludeGuard");

   //  This warning is logged against the #ifndef.
   //
   auto ifn = CurrBegin(log.Pos());
   if(ifn == string::npos) return NotFound("Position of #define");
   if(!IsDirective(ifn, HASH_IFNDEF_STR))
      return NotFound(HASH_IFNDEF_STR);
   auto guard = log.File()->MakeGuardName();
   ifn += strlen(HASH_IFNDEF_STR) + 1;
   auto end = CurrEnd(ifn) - 1;
   Erase(ifn, end - ifn + 1);
   Insert(ifn, guard);
   auto def = Find(ifn, HASH_DEFINE_STR);
   if(def == string::npos) return NotFound(HASH_DEFINE_STR);
   def += strlen(HASH_DEFINE_STR) + 1;
   end = CurrEnd(def) - 1;
   Erase(def, end - def + 1);
   Insert(def, guard);
   return Changed(def);
}

//------------------------------------------------------------------------------

size_t Editor::Replace(size_t pos, size_t count, const std::string& code)
{
   Debug::ft("Editor.Replace");

   Erase(pos, count);
   Insert(pos, code);
   return pos;
}

//------------------------------------------------------------------------------

word Editor::ReplaceHeading(const CodeWarning& log)
{
   Debug::ft("Editor.ReplaceHeading");

   //  Remove the existing header and replace it with the standard one,
   //  inserting the file name where appropriate.
   //
   return Unimplemented();
}

//------------------------------------------------------------------------------

word Editor::ReplaceName(const CodeWarning& log)
{
   Debug::ft("Editor.ReplaceName");

   //  Prompt for a new name that will replace the existing one.
   //
   return Unimplemented();
}

//------------------------------------------------------------------------------

word Editor::ReplaceNull(const CodeWarning& log)
{
   Debug::ft("Editor.ReplaceNull");

   //  If there are multiple occurrences on the same line, each one will
   //  cause a log, so just fix the first one.
   //
   auto begin = CurrBegin(log.Pos());
   if(begin == string::npos) return NotFound("Position of NULL");
   auto null = FindWord(begin, NULL_STR);
   if(null == string::npos) return NotFound(NULL_STR, true);
   Replace(null, strlen(NULL_STR), NULLPTR_STR);
   return Changed(null);
}

//------------------------------------------------------------------------------

word Editor::ReplaceSlashAsterisk(const CodeWarning& log)
{
   Debug::ft("Editor.ReplaceSlashAsterisk");

   auto pos0 = CurrBegin(log.Pos());
   if(pos0 == string::npos) return NotFound("Position of /*");
   auto pos1 = source_.find(COMMENT_BEGIN_STR, pos0);
   if(pos1 == string::npos) return NotFound(COMMENT_BEGIN_STR);
   auto pos2 = LineFind(pos1, COMMENT_END_STR);
   auto pos3 = LineFindNext(pos1 + strlen(COMMENT_BEGIN_STR));
   auto pos4 = (pos2 == string::npos ? string::npos :
      LineFindNext(pos2 + strlen(COMMENT_END_STR)));

   //  We now have
   //  o pos1: start of /*
   //  o pos2: start of */ (if on this line)
   //  o pos3: first non-blank following /* (if any)
   //  o pos4: first non-blank following */ (if any)
   //
   //  The scenarios are                                        pos2 pos3 pos4
   //  1: /*         erase /* and continue                        -    -    -
   //  2: /* aaa     replace /* with // and continue              -    *    -
   //  3: /* aaa */  replace /* with // and erase */ and return   *    *    -
   //  4: /* */ aaa  return                                       *    *    *
   //
   if(pos4 != string::npos)  // [4]
   {
      return Report("Unchanged: code follows /*...*/");
   }
   else if(pos3 == string::npos)  // [1]
   {
      Erase(pos1, strlen(COMMENT_BEGIN_STR));
      Changed();
   }
   else if((pos2 == string::npos) && (pos3 != string::npos))  // [2]
   {
      source_.replace(pos1, strlen(COMMENT_BEGIN_STR), COMMENT_STR);
      Changed();
   }
   else  // [3]
   {
      Erase(pos2, strlen(COMMENT_END_STR));
      source_.replace(pos1, strlen(COMMENT_BEGIN_STR), COMMENT_STR);
      return Changed(pos1);
   }

   //  Subsequent lines will be commented with "//", which will be indented
   //  appropriately and followed by two spaces.
   //
   auto info = GetLineInfo(pos0);
   auto comment = spaces(info->depth * IndentSize());
   comment += COMMENT_STR + spaces(2);

   //  Now continue with subsequent lines:
   //  o pos0: start of the current line
   //  o pos2: start of */ (if on this line)
   //  o pos3: first non-blank after start of line and preceding */
   //  o pos4: first non-blank following */ (if any)
   //
   //  The scenarios are                                    pos2 pos3 pos4
   //  1: no */       prefix // and continue                  -
   //  2: */          erase */ and return                     *    -    -
   //  3: */ aaa      replace */ with line break and return   *    -    *
   //  4: aaa */      prefix // and erase */ and return       *    *    -
   //  5: aaa */ aaa  prefix // and replace */ with line      *    *    *
   //                 break and return
   //
   for(pos0 = NextBegin(pos0); pos0 != string::npos; pos0 = NextBegin(pos0))
   {
      pos2 = LineFind(pos0, COMMENT_END_STR);
      pos3 = LineFindNext(pos0);
      if(pos3 == pos2) pos3 = string::npos;

      if(pos2 != string::npos)
         pos4 = LineFindNext(pos2 + strlen(COMMENT_END_STR));
      else
         pos4 = string::npos;

      if(pos2 == string::npos)  // [1]
      {
         InsertPrefix(pos0, comment);
         Changed();
      }
      else if((pos3 == string::npos) && (pos4 == string::npos))  // [2]
      {
         Erase(pos2, strlen(COMMENT_END_STR));
         return Changed(PrevBegin(pos2));
      }
      else if((pos3 == string::npos) && (pos4 != string::npos))  // [3]
      {
         Erase(pos2, strlen(COMMENT_END_STR));
         Changed();
         InsertLineBreak(pos2);
         return Changed(PrevBegin(pos2));
      }
      else if((pos3 != string::npos) && (pos4 == string::npos))  // [4]
      {
         Erase(pos2, strlen(COMMENT_END_STR));
         InsertPrefix(pos0, comment);
         return Changed(pos0);
      }
      else  // [5]
      {
         Erase(pos2, strlen(COMMENT_END_STR));
         InsertLineBreak(pos2);
         InsertPrefix(pos0, comment);
         return Changed(pos0);
      }
   }

   return Report("Closing */ not found. Inspect changes!");
}

//------------------------------------------------------------------------------

word Editor::ReplaceUsing(const CodeWarning& log)
{
   Debug::ft("Editor.ReplaceUsing");

   //  Before removing the using statement, add type aliases to each class
   //  for symbols that appear in its definition and that were resolved by
   //  a using statement.
   //
   ResolveUsings();
   return EraseCode(log.item_);
}

//------------------------------------------------------------------------------
//
//  Displays EXPL when RC was returned after fixing LOG.
//
void Editor::ReportFix(CliThread& cli, CodeWarning* log, word rc)
{
   Debug::ft("Editor.ReportFix");

   auto expl = GetExpl();

   if(rc < EditCompleted)
   {
      *cli.obuf << spaces(2) << (expl.empty() ? SuccessExpl : expl);
      if(expl.empty() || (expl.back() != CRLF)) *cli.obuf << CRLF;
      cli.Flush();
   }

   if((log != nullptr) && (rc == EditSucceeded)) log->status_ = Pending;
}

//------------------------------------------------------------------------------

word Editor::ResolveUsings()
{
   Debug::ft("Editor.ResolveUsings");

   //  This function only needs to be run once per file.
   //
   if(aliased_) return EditSucceeded;

   //  Classes, data, functions, and typedefs can contain names resolved by
   //  using statements.  Modify each of these so that using statements can
   //  be removed.
   //
   auto classes = file_->Classes();

   for(auto c = classes->cbegin(); c != classes->cend(); ++c)
   {
      auto refs = FindUsingReferents(*c);

      for(auto r = refs.cbegin(); r != refs.cend(); ++r)
      {
         QualifyReferent(*c, *r);
      }
   }

   //  Qualify names in non-class items at file scope.
   //
   auto data = file_->Datas();
   for(auto d = data->cbegin(); d != data->cend(); ++d)
   {
      QualifyUsings(*d);
   }

   auto funcs = file_->Funcs();
   for(auto f = funcs->cbegin(); f != funcs->cend(); ++f)
   {
      QualifyUsings(*f);
   }

   auto types = file_->Types();
   for(auto t = types->cbegin(); t != types->cend(); ++t)
   {
      QualifyUsings(*t);
   }

   aliased_ = true;
   return EditSucceeded;
}

//------------------------------------------------------------------------------

void Editor::Setup(CodeFile* file)
{
   Debug::ft("Editor.Setup");

   if((file_ != nullptr) || (file == nullptr)) return;

   //  Get the file's code and configure the lexer.  The code is read in
   //  again because Lexer.Preprocess has modified the original version.
   //
   file_ = file;
   file_->ReadCode(source_);
   Initialize(source_, file_);
   CalcDepths();
   CalcLineTypes(false);

   //  Get the file's warnings and sort them for fixing.  The order reduces
   //  the chances of an item's position changing before it is edited.
   //
   CodeWarning::GetWarnings(file_, warnings_);
   std::sort(warnings_.begin(), warnings_.end(), CodeWarning::IsSortedToFix);
}

//------------------------------------------------------------------------------

word Editor::SortIncludes()
{
   Debug::ft("Editor.SortIncludes");

   //  Start by mangling all #includes, which makes them easier to sort.
   //
   MangleIncludes();

   //  Copy all of the #includes into a list and sort them.  Look at the
   //  entire file in case any #include appears outside the initial list.
   //
   std::list< string > includes;

   for(auto pos = 0; pos != string::npos; pos = NextBegin(pos))
   {
      if(IsDirective(pos, HASH_INCLUDE_STR))
      {
         includes.push_back(GetCode(pos));
      }
   }

   if(includes.empty()) return NotFound(HASH_INCLUDE_STR);
   includes.sort(IncludesAreSorted);

   //  Run through the #includes.  When one doesn't match the sort order,
   //  find the correct one, cut it, and paste it into the current location.
   //
   auto targ = includes.cbegin();

   for(auto pos = IncludesBegin();
      (pos != string::npos) && (targ != includes.cend());
      pos = NextBegin(pos), ++targ)
   {
      if(GetCode(pos) != *targ)
      {
         auto from = FindAndCutInclude(pos, *targ);

         if(from != string::npos)
         {
            Paste(pos, *targ, from);
         }
         else
         {
            auto err = string("Failed to find ") + *targ;
            return Report(err.c_str());
         }
      }
   }

   sorted_ = true;
   Changed();
   return Report("All #includes sorted.", EditSucceeded);
}

//------------------------------------------------------------------------------

word Editor::SplitVirtualFunction(const Function* func)
{
   Debug::ft("Editor.SplitVirtualFunction");

   //  Split this public virtual function:
   //  o Rename the function, its overrides, and its invocations within
   //    overrides to its original name + "_v".
   //  o Make its declaration protected and virtual.
   //  o Make its public declaration non-virtual, with the implementation
   //    simply invoking its renamed, protected version.
   //
   return Unimplemented();
}

//------------------------------------------------------------------------------

word Editor::TagAsConstArgument(const Function* func, word offset)
{
   Debug::ft("Editor.TagAsConstArgument");

   //  Find the line on which the argument's type appears, and insert
   //  "const" before that type.
   //
   auto index = func->LogOffsetToArgIndex(offset);
   auto type = func->GetArgs().at(index)->GetTypeSpec()->GetPos();
   Insert(type, "const ");
   return Changed(type);
}

//------------------------------------------------------------------------------

word Editor::TagAsConstData(const CodeWarning& log)
{
   Debug::ft("Editor.TagAsConstData");

   //  Find the line on which the data's type appears, and insert
   //  "const" before that type.
   //
   auto type = log.item_->GetTypeSpec()->GetPos();
   if(type == string::npos) return NotFound("Data type");
   Insert(type, "const ");
   return Changed(type);
}

//------------------------------------------------------------------------------

word Editor::TagAsConstFunction(const Function* func)
{
   Debug::ft("Editor.TagAsConstFunction");

   //  Insert " const" after the right parenthesis at the end of the function's
   //  argument list.
   //
   auto endsig = FindArgsEnd(func);
   if(endsig == string::npos) return NotFound("End of argument list");
   Insert(endsig + 1, " const");
   return Changed(endsig);
}

//------------------------------------------------------------------------------

word Editor::TagAsConstPointer(const CodeWarning& log)
{
   Debug::ft("Editor.TagAsConstPointer");

   //  If there is more than one pointer, this applies to the last one, so
   //  back up from the data item's name.  Search for the name on this line
   //  in case other edits have altered its position.
   //
   auto data = log.item_->GetPos();
   if(data == string::npos) return NotFound("Data member");
   auto name = FindWord(data, log.item_->Name());
   if(name == string::npos) return NotFound("Member name");
   auto ptr = Rfind(name, "*");
   if(ptr == string::npos) return NotFound("Pointer tag");
   Insert(ptr + 1, " const");
   return Changed(ptr);
}

//------------------------------------------------------------------------------

word Editor::TagAsConstReference(const Function* func, word offset)
{
   Debug::ft("Editor.TagAsConstReference");

   //  Find the line on which the argument's name appears.  Insert a reference
   //  tag before the argument's name and "const" before its type.  The tag is
   //  added first so that its position won't change as a result of adding the
   //  "const" earlier in the line.
   //
   auto& args = func->GetArgs();
   auto index = func->LogOffsetToArgIndex(offset);
   auto arg = args.at(index).get();
   if(arg == nullptr) return NotFound("Argument");
   auto pos = arg->GetPos();
   if(pos == string::npos) return NotFound("Argument name");
   auto prev = RfindNonBlank(pos - 1);
   Insert(prev + 1, "&");
   auto rc = TagAsConstArgument(func, offset);
   if(rc != EditSucceeded) return rc;
   Expl_.clear();
   return Changed(prev);
}

//------------------------------------------------------------------------------

word Editor::TagAsDefaulted(const Function* func)
{
   Debug::ft("Editor.TagAsDefaulted");

   //  If this is a separate definition, delete it.
   //
   if(func->GetDecl() != func)
   {
      return EraseCode(func);
   }

   //  This is the function's declaration, and possibly its definition.
   //
   auto endsig = FindSigEnd(func);
   if(endsig == string::npos) return NotFound("Signature end");
   if(source_[endsig] == ';')
   {
      Insert(endsig, " = default");
   }
   else
   {
      auto right = FindFirstOf(endsig + 1, "}");
      if(right == string::npos) return NotFound("Right brace");
      Erase(endsig, right - endsig + 1);
      Insert(endsig, "= default;");
   }

   return Changed(endsig);
}

//------------------------------------------------------------------------------

word Editor::TagAsExplicit(const CodeWarning& log)
{
   Debug::ft("Editor.TagAsExplicit");

   //  A constructor can be tagged "constexpr", which the parser looks for
   //  only *after* "explicit".
   //
   auto ctor = log.item_->GetPos();
   if(ctor == string::npos) return NotFound("Constructor");
   auto prev = LineRfind(ctor, CONSTEXPR_STR);
   if(prev != string::npos) ctor = prev;
   Insert(ctor, "explicit ");
   return Changed(ctor);
}

//------------------------------------------------------------------------------

word Editor::TagAsNoexcept(const Function* func)
{
   Debug::ft("Editor.TagAsNoexcept");

   //  Insert "noexcept" after "const" but before "override" or "final".
   //  Start by finding the end of the function's argument list.
   //
   auto pos = func->GetPos();
   if(pos == string::npos) return NotFound("Function name");
   auto rpar = FindArgsEnd(func);
   if(rpar == string::npos) return NotFound("End of argument list");

   //  If there is a "const" after the right parenthesis, insert "noexcept"
   //  after it, else insert "noexcept" after the right parenthesis.
   //
   auto cons = FindNonBlank(rpar + 1);
   if(CodeMatches(cons, CONST_STR))
   {
      Insert(cons + strlen(CONST_STR), " noexcept");
      return Changed(cons);
   }

   Insert(rpar + 1, " noexcept");
   return Changed(rpar);
}

//------------------------------------------------------------------------------

word Editor::TagAsOverride(const CodeWarning& log)
{
   Debug::ft("Editor.TagAsOverride");

   //  Remove the function's virtual tag, if any.
   //
   auto rc = EraseVirtualTag(log);
   if(rc != EditSucceeded) return rc;
   Expl_.clear();

   //  Insert "override" after the last non-blank character at the end
   //  of the function's signature.
   //
   auto endsig = FindSigEnd(log);
   if(endsig == string::npos) return NotFound("Signature end");
   endsig = RfindNonBlank(endsig - 1);
   Insert(endsig + 1, " override");
   return Changed(endsig);
}

//------------------------------------------------------------------------------

word Editor::TagAsStaticFunction(const Function* func)
{
   Debug::ft("Editor.TagAsStaticFunction");

   //  Start with the function's return type in case it's on the line above
   //  the function's name.  Then find the end of the argument list.
   //
   auto type = func->GetTypeSpec()->GetPos();
   if(type == string::npos) return NotFound("Function type");
   auto rpar = FindArgsEnd(func);
   if(rpar == string::npos) return NotFound("End of argument list");

   //  Only a function's declaration, not its definition, is tagged "static".
   //  Start by removing any "virtual" tag.  Then put "static" after "extern"
   //  and/or "inline".
   //
   if(func->GetDecl() == func)
   {
      auto front = LineRfind(type, VIRTUAL_STR);
      if(front != string::npos) Erase(front, strlen(VIRTUAL_STR) + 1);
      front = LineRfind(type, INLINE_STR);
      if(front != string::npos) type = front;
      front = LineRfind(type, EXTERN_STR);
      if(front != string::npos) type = front;
      Insert(type, "static ");
      Changed();
   }

   //  A static function cannot be const, so remove that tag if it exists.  If
   //  "const" is on the same line as RPAR, delete any space *before* "const";
   //  if it's on the next line, delete any space *after* "const" to preserve
   //  indentation.
   //
   if(func->IsConst())
   {
      auto tag = FindWord(rpar, CONST_STR);

      if(tag != string::npos)
      {
         Erase(tag, strlen(CONST_STR));

         if(OnSameLine(rpar, tag))
         {
            if(IsBlank(source_[tag - 1])) Erase(tag - 1, 1);
         }
         else
         {
            if(IsBlank(source_[tag])) Erase(tag, 1);
         }
      }
   }

   return Changed(type);
}

//------------------------------------------------------------------------------

word Editor::TagAsVirtual(const CodeWarning& log)
{
   Debug::ft("Editor.TagAsVirtual");

   //  Make this destructor virtual.
   //
   auto pos = Insert(log.item_->GetPos(), "virtual ");
   return Changed(pos);
}

//------------------------------------------------------------------------------

word Editor::UpdateFuncDefnAttrs
   (const Function* func, FuncDefnAttrs& attrs) const
{
   Debug::ft("Editor.UpdateFuncDefnAttrs");

   if(func == nullptr) return EditContinue;

   //  See if FUNC is preceded or followed by a rule, or preceded
   //  *and* followed by a blank line.
   //
   bool before = false;
   bool after = false;
   size_t begin, left, end;
   if(!func->GetRange(begin, left, end)) return NotFound("Adjacent function");
   auto pos = NextBegin(end);
   auto type = PosToType(pos);

   if(type == BlankLine)
   {
      before = true;
      type = PosToType(NextBegin(pos));
      if(type == RuleComment)
      {
         attrs.rule = true;
         attrs.blank = BlankBefore;
         return EditContinue;
      }
   }

   pos = begin;
   while(true)
   {
      pos = PrevBegin(pos);
      type = PosToType(pos);

      switch(type)
      {
      case RuleComment:
         attrs.rule = true;
         attrs.blank = BlankBefore;
         return EditContinue;

      case BlankLine:
         after = true;
         //  [[fallthrough]]
      case FunctionName:
         continue;

      default:
         if(before && after) attrs.blank = BlankBefore;
         return EditContinue;
      }
   }
}

//------------------------------------------------------------------------------

word Editor::UpdateFuncDefnLoc
   (const Function* prev, const Function* next, FuncDefnAttrs& attrs) const
{
   Debug::ft("Editor.UpdateFuncDefnLoc");

   //  PREV and NEXT are the functions that precede and follow the function
   //  whose definition is to be inserted.
   //
   auto rc = UpdateFuncDefnAttrs(prev, attrs);
   if(rc != EditContinue) return rc;
   rc = UpdateFuncDefnAttrs(next, attrs);
   if(rc != EditContinue) return rc;

   if(prev != nullptr)
   {
      //  Insert the function after the one that precedes it.
      //
      attrs.pos = LineAfterItem(prev);
      return EditContinue;
   }

   if(next == nullptr)
   {
      //  Insert a rule above the function.  There must be something else
      //  in the file!
      //
      attrs.rule = true;
      attrs.blank = BlankBefore;

      //  Insert the function at the bottom of the file.  If it ends with
      //  two closing braces, the second one ends a namespace definition,
      //  so insert the definition above that one.
      //
      auto pos = PrevBegin(string::npos);
      auto type = PosToType(pos);
      if(type != CloseBrace) return NotFound("File's second last }");
      type = PosToType(PrevBegin(pos));
      if(type != CloseBrace) return NotFound("File's last }");
      attrs.pos = pos;
      return EditContinue;
   }

   //  Insert the function before NEXT.  If NEXT has an fn_name, insert the
   //  definition above *that*.  If the new function is to be offset with a
   //  blank and/or rule, they need to go *after* the new function.
   //
   if(attrs.blank != BlankNone) attrs.blank = BlankAfter;

   auto pred = PrevBegin(next->GetPos());

   while(true)
   {
      auto type = PosToType(pred);

      switch(type)
      {
      case BlankLine:
         pred = PrevBegin(pred);
         continue;
      case FunctionName:
         while(PosToType(--pred) == FunctionName);
         attrs.pos = ++pred;
         return EditContinue;
      default:
         break;
      }
   }

   attrs.pos = pred;
   return EditContinue;
}

//------------------------------------------------------------------------------

word Editor::UpdateItemControls(const Class* cls, ItemDeclAttrs& attrs) const
{
   Debug::ft("Editor.UpdateItemControls");

   //  Determine the access control that will be in effect at attrs.pos.
   //  If we reach an access control that is more restrictive than the
   //  one for the item, insert the item directly above it.
   //
   size_t begin, left, end;
   if(!cls->GetRange(begin, left, end))
   {
      return NotFound("Item's class");
   }

   auto prev =
      (cls->GetClassTag() == Cxx::ClassType ? Cxx::Private : Cxx::Public);

   for(auto pos = CurrBegin(begin); pos <= attrs.pos; pos = NextBegin(pos))
   {
      auto code = GetCode(pos);
      auto acc = FindAccessControl(code);

      if(acc != Cxx::Access_N)
      {
         if(acc < attrs.access)
         {
            attrs.pos = pos;
            attrs.thisctrl = (prev != attrs.access);
            if(attrs.thisctrl) attrs.blank = BlankNone;
            if(attrs.blank == BlankAfter) attrs.blank = BlankBefore;
            return EditContinue;
         }

         prev = acc;
      }
   }

   if(prev != attrs.access)
   {
      //  The item's access control is not in effect at the insertion point,
      //  so it must be inserted, and the access control that was in effect
      //  must be inserted after the item unless we've reached the end of
      //  the class.
      //
      attrs.thisctrl = true;
      if(attrs.blank == BlankBefore) attrs.blank = BlankAfter;
      if(PosToType(attrs.pos) != CloseBraceSemicolon) attrs.nextctrl = prev;
   }
   else
   {
      //  If the item is being insert *at* its access control, insert it
      //  immediately after that control.
      //
      if(FindAccessControl(GetCode(attrs.pos)) == attrs.access)
      {
         attrs.pos = NextBegin(attrs.pos);
         if(attrs.blank == BlankBefore) attrs.blank = BlankAfter;
      }
   }

   return EditContinue;
}

//------------------------------------------------------------------------------

word Editor::UpdateItemDeclAttrs
   (const CxxToken* item, ItemDeclAttrs& attrs) const
{
   Debug::ft("Editor.UpdateItemDeclAttrs");

   if(item == nullptr) return EditContinue;

   size_t begin, left, end;
   if(!item->GetRange(begin, left, end)) return NotFound("Adjacent item");

   //  Indent the code to match that of ITEM.
   //
   attrs.indent = LineFindFirst(begin) - CurrBegin(begin);

   //  If ITEM is commented, a blank line should also precede or follow it.
   //
   auto type = PosToType(PrevBegin(begin));
   auto& line = LineTypeAttr::Attrs[type];

   if(!line.isCode && (type != BlankLine))
   {
      attrs.comment = true;
      attrs.blank = BlankBefore;
      return EditContinue;
   }

   //  ITEM isn't commented, so see if a blank line precedes and follows it.
   //
   if((type == BlankLine) && (PosToType(NextBegin(end)) == BlankLine))
   {
      attrs.blank = BlankBefore;
   }

   return EditContinue;
}

//------------------------------------------------------------------------------

fn_name Editor_UpdateItemDeclLoc = "Editor.UpdateItemDeclLoc";

word Editor::UpdateItemDeclLoc(const Class* cls,
   const CxxToken* prev, const CxxToken* next, ItemDeclAttrs& attrs) const
{
   Debug::ft(Editor_UpdateItemDeclLoc);

   //  PREV and NEXT are the items that precede and follow the item whose
   //  declaration is to be inserted.
   //
   auto rc = UpdateItemDeclAttrs(prev, attrs);
   if(rc != EditContinue) return rc;
   rc = UpdateItemDeclAttrs(next, attrs);
   if(rc != EditContinue) return rc;

   //  Special member functions are added in order of FunctionRole, so add
   //  them bottom-up so that they will appear in the desired order.
   //
   if(next == nullptr)
   {
      //  Insert the item before the class's final closing brace.
      //
      size_t begin, left, end;
      if(!cls->GetRange(begin, left, end)) return NotFound("Item's class");
      attrs.pos = CurrBegin(end);
      if((prev == nullptr) && (next == nullptr)) attrs.comment = true;
      if(attrs.blank == BlankAfter) attrs.blank = BlankBefore;
      return EditContinue;
   }

   //  Insert the item before NEXT.  If the item is to be set off with a
   //  blank, the blank must follow it.
   //
   if(attrs.blank != BlankNone) attrs.blank = BlankAfter;

   auto pred = PrevBegin(next->GetPos());

   while(true)
   {
      auto type = PosToType(pred);

      if(!LineTypeAttr::Attrs[type].isCode)
      {
         if(type == BlankLine) break;

         //  This is a comment, so keep moving up to find the insertion point.
         //
         pred = PrevBegin(pred);
         continue;
      }

      if(type == AccessControl)
      {
         if(FindAccessControl(GetCode(pred)) != attrs.access)
         {
            //  This isn't the desired control.  Insert the item here or
            //  above; UpdateItemControls will set the exact position.
            //
            attrs.pos = pred;
            return UpdateItemControls(next->GetClass(), attrs);
         }

         //  This is the desired access control.  Insert the item after
         //  it so that it can be reused.
         //
         attrs.pos = NextBegin(pred);
         return EditContinue;
      }

      break;
   }

   //  Insert the item above the line that follows PRED.
   //
   attrs.pos = NextBegin(pred);
   return UpdateItemControls(next->GetClass(), attrs);
}

//------------------------------------------------------------------------------

void Editor::UpdateWarnings
   (EditorAction action, size_t begin, size_t count, size_t from) const
{
   Debug::ft("Editor.UpdateWarnings");

   for(auto w = warnings_.begin(); w != warnings_.end(); ++w)
   {
      (*w)->UpdatePos(action, begin, count, from);
   }
}

//------------------------------------------------------------------------------

word Editor::Write()
{
   Debug::ft("Editor.Write");

   std::ostringstream stream;

   //  Create a new file to hold the reformatted version.
   //
   auto path = file_->Path();
   auto temp = path + ".tmp";
   auto output = SysFile::CreateOstream(temp.c_str(), true);
   if(output == nullptr)
   {
      stream << "Failed to open output file for " << file_->Name();
      return Report(stream, EditAbort);
   }

   //  #includes for files that define base classes used in this file or declare
   //  functions defined in this file had their angle brackets or quotes mangled
   //  for sorting purposes.  Fix this.
   //
   for(size_t pos = 0; pos != string::npos; pos = NextBegin(pos))
   {
      if(IsDirective(pos, HASH_INCLUDE_STR))
      {
         auto incl = GetCode(pos);
         DemangleInclude(incl);
         source_.erase(pos, incl.size());
         source_.insert(pos, incl);
      }
   }

   *output << source_;

   //  Delete the original file and replace it with the new one.
   //
   output.reset();
   auto err = remove(path.c_str());

   if(err != 0)
   {
      stream << "Failed to remove " << file_->Name() << ": error=" << err;
      return Report(stream, EditAbort);
   }

   err = rename(temp.c_str(), path.c_str());

   if(err != 0)
   {
      stream << "Failed to rename " << file_->Name() << ": error=" << err;
      return Report(stream, EditAbort);
   }

   for(auto w = warnings_.begin(); w != warnings_.end(); ++w)
   {
      if((*w)->status_ == Pending) (*w)->status_ = Fixed;
   }

   stream << "..." << file_->Name() << " committed";
   return Report(stream, EditCompleted);
}
}
