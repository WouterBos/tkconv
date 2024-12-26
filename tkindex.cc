#include <fmt/format.h>
#include <fmt/printf.h>
#include <fmt/ranges.h>
#include <regex>
#include <mutex>
#include <iostream>
#include "sqlwriter.hh"
#include <atomic>
#include "support.hh"
#include <unordered_set>
#include "argparse/argparse.hpp"

using namespace std;

static string textFromFile(const std::string& fname)
{
  string command;
  if(isPDF(fname)) {
    command = string("pdftotext -q -nopgbrk - < '") + fname + "' -";
  }
  else if(isDocx(fname)) {
    command = string("pandoc -f docx '"+fname+"' -t plain");
  }
  else if(isXML(fname)) {
    command = string("xmlstarlet tr tk.xslt < '"+fname+"' | sed 's:<[^>]*>: :g'");
  }
  else if(isDoc(fname))
    command = "catdoc - < '" + fname +"'";
  else if(isRtf(fname))
    command = string("pandoc -f rtf '"+fname+"' -t plain");
  else
    return "";
  
  string ret;
  FILE* pfp = popen(command.c_str(), "r");
  if(!pfp)
    throw runtime_error("Unable to perform pdftotext: "+string(strerror(errno)));
  shared_ptr<FILE> fp(pfp, pclose);
  
  char buffer[4096];
  
  for(;;) {
    int len = fread(buffer, 1, sizeof(buffer), fp.get());
    if(!len)
      break;
    ret.append(buffer, len);
  }
  if(ferror(fp.get()))
    throw runtime_error("Unable to perform pdftotext: "+string(strerror(errno)));
  return ret;
}

/* The story:
   The ground truth are the Document and Verslag tables.
   Then there is the storage of documents on disk
   Then there is the docsearch table with the search index, which is fast for search and slow for scanning
   Then there is the 'indexed' table which we try to keep in sync with docsearch

   We start by checking if all known Documents have the right size on disk, otherwise we remove the document from the index, and retrieve a fresh version

   We can regenerate the 'indexed' table from docsearch.

   If a document disappears from Document of Verslag, we do nothing

   Each Vergadering has multiple Verslag-en. We are only interested in the newest Verslag. The other copies need to be removed. 

*/

int main(int argc, char** argv)
{
  argparse::ArgumentParser args("tkindex", "0.0");

  args.add_argument("--begin")
    .help("Begin date of indexing, 2024-12-05 format").default_value("2008-01-01");
  args.add_argument("--tkindex")
    .help("filename that holds our index").default_value("tkindex.sqlite3");
  args.add_argument("--cleanup").default_value(false)
    .implicit_value(true).help("Cleanup older documents");
  
  args.add_argument("--days")
    .default_value(-1)
    .help("Number of days of history to index")
    .scan<'i', int>();
  
  try {
    args.parse_args(argc, argv);
  }
  catch (const std::runtime_error& err) {
    std::cout << err.what() << std::endl << args;
    std::exit(1);
  }
  string limit = args.get<string>("begin");
  
  if (args.get<int>("--days") > 0) {
    int days = args.get<int>("--days");
    cout << "Number of days set: "<< days << endl;
    limit = getTimeDBFormat(time(0) - days * 86400);
  }
  cout<<"Limit for documents: "<<limit<<endl;
  SQLiteWriter todo("tk.sqlite3", SQLWFlag::ReadOnly);

  std::regex dregex(R"(\d{4}-\d{2}-\d{2})");
  if(!regex_match(limit, dregex)) {
    fmt::print("The configured begin limit does not look like a date: '{}' (should be 2024-12-25)\n", limit);
    return EXIT_FAILURE;
  }

  fmt::print("Getting document ids from database since {}\n", limit);
  auto wantDocs = todo.queryT("select id,titel,onderwerp,datum,'Document' as category, contentLength from Document where datum > ?", {limit});

  fmt::print("There are {} documents in the database that need to be indexed\n", wantDocs.size());

  // query voor verslagen is ingewikkeld want we willen alleen de nieuwste versie indexeren
  // en sterker nog alle oude versies wissen
  fmt::print("Getting verslagen since {}\n", limit);
  auto alleVerslagen = todo.queryT("select Verslag.id as id, vergadering.id as vergaderingid,datum, vergadering.titel as onderwerp, '' as titel, 'Verslag' as category, contentLength from Verslag,Vergadering where Verslag.vergaderingId=Vergadering.id and datum > ? order by datum desc, verslag.updated desc", {limit});

  set<string> seenvergadering;
  decltype(alleVerslagen) wantVerslagen;
  for(auto& v: alleVerslagen) {
    string vid = get<string>(v["vergaderingid"]);
    if(seenvergadering.count(vid)) 
      continue;
    wantVerslagen.push_back(v);
    seenvergadering.insert(vid);
  }
  fmt::print("Would like to index {} most recent verslagen\n", wantVerslagen.size());

  string idxfname = args.get<string>("--tkindex");
  fmt::print("tkindex filename: {}\n", idxfname);
  SQLiteWriter sqlw(idxfname, {{"indexed", {{"uuid", "PRIMARY KEY"}}}});

  sqlw.queryT(R"(
CREATE VIRTUAL TABLE IF NOT EXISTS docsearch USING fts5(onderwerp, titel, tekst, contentLength UNINDEXED, uuid UNINDEXED, datum UNINDEXED, category UNINDEXED,  tokenize="unicode61 tokenchars '_'")
)");

  // IF THIS GETS OUT OF SYNC, drop 'indexed', and it will be recreated automatically:
  sqlw.queryT("create table if not exists indexed as select datum,uuid,contentLength,category from docsearch");
  sqlw.queryT("create unique index if not exists uuididx on indexed(uuid)");


  if (args["--cleanup"] == true) {
    fmt::print("Cleaning up documents that are older than {}\n", limit);
    sqlw.queryT("delete from indexed where datum < ?", {limit});
    sqlw.queryT("delete from docsearch where datum < ?", {limit});
  }

  
  fmt::print("Retrieving already indexed document uuids from 'indexed' table..");
  cout.flush();
  auto already = sqlw.queryT("select uuid,contentLength from indexed");
  map<string, int64_t> skipids; // ordering actually gets us locality of reference below

  for(auto& a : already) {
    skipids[get<string>(a["uuid"])] = get<int64_t>(a["contentLength"]);
  }
  fmt::print(" got {}\n", skipids.size());
  
  // next up, check if there are indexed documents that are not in docs or verslagen
  set<string> exists;
  for(auto& e : wantDocs) {
    exists.insert(eget(e, "id"));
  }
  for(auto& e : wantVerslagen) {
    exists.insert(eget(e, "id"));
  }

  sqlw.queryT("ATTACH DATABASE ':memory:' AS aux1");
  bool workToDo=false;
  for(const auto& si : skipids) {
    if(!exists.count(si.first)) {
      cout<<si.first<<" is in indexed, but no longer in Document or Verslag table, will be removed\n";
      sqlw.addValue({{"id", si.first}}, "aux1.todel");
      workToDo = true;
    }
  }

  

  unordered_set<string> dropids, reindex;
  fmt::print("Checking for {} already indexed documents if they have the right size on disk\n", skipids.size());
  for(const auto& si : skipids) {
    if(!isPresentNonEmpty(si.first)) {
      fmt::print("We miss document enclosure for indexed document with id {}\n", si.first);
      dropids.insert(si.first); 
    }
    else if(!isPresentRightSize(si.first, si.second)) {
      fmt::print("Document enclosure for indexed document with id {} is wrong size, reindexing\n", si.first);
      reindex.insert(si.first); 
    }
  }
  fmt::print("{} entries that are indexed have no file enclosure present\n", dropids.size());
  fmt::print("{} entries that are indexed have incorrectly sized enclosure, reindexing\n", reindex.size());

  for(const auto& di : dropids) {
    fmt::print("Will remove absent {} from index\n", di);
    sqlw.addValue({{"id", di}}, "aux1.todel");
    workToDo = true;
  }
  int remcount=1;
  for(const auto& di : reindex) {
    fmt::print("Will remove wrongly sized {} from index ({}/{})\n", di, remcount, reindex.size());
    remcount++;
    sqlw.addValue({{"id", di}}, "aux1.todel");
    workToDo = true;
    skipids.erase(di);
  }

  if(workToDo) {
    fmt::print("Now actually going to delete entries from the search index\n");
    sqlw.queryT("delete from docsearch where uuid in (select * from aux1.todel)");
    fmt::print("Now actually going to delete entries from the parallel index\n");
    sqlw.queryT("delete from indexed where uuid in (select * from aux1.todel)");
  }

  
  fmt::print("{} documents are already indexed & will be skipped\n",
	     skipids.size());

  decltype(wantDocs) wantAll = wantDocs;

  for(const auto& wv : wantVerslagen)
    wantAll.push_back(wv);
  
  atomic<size_t> ctr = 0;

  std::mutex m;
  atomic<int> skipped=0, notpresent=0, wrong=0, indexed=0;
  auto worker = [&]() {
    for(unsigned int n = ctr++; n < wantAll.size(); n = ctr++) {
      string id = get<string>(wantAll[n]["id"]);
      if(skipids.count(id)) {
	//	fmt::print("{} indexed already, skipping\n", id);
	skipped++;
	continue;
      }
      string fname = makePathForId(id);
      if(!isPresentNonEmpty(id)) {
	//	fmt::print("{} is not present\n", id);
	notpresent++;
	continue;
      }
      string text = textFromFile(fname);
      
      if(text.empty()) {
	if(isPresentNonEmpty(id, "improvdocs")) {
	  string impfname = makePathForId(id, "improvdocs");
	  
	  text = textFromFile(impfname);
	  if(!text.empty()) {
	    fmt::print("{} did work using improvdocs overlay!\n", id);
	  }
	  else {
	    fmt::print("{} is not a file we can deal with {}\n", fname, isPDF(impfname) ? "PDF" : "");
	    wrong++;
	    continue;
	  }
	}
	else {
	  fmt::print("{} is not a file we can deal with {}\n", fname, isPDF(fname) ? "PDF" : "");
	  wrong++;
	  continue;
	}
      }

      lock_guard<mutex> p(m);
      string titel;
      try {
	titel = 	  get<string>(wantAll[n]["titel"]);
      } catch(...){}
      
      sqlw.queryT("insert into docsearch values (?,?,?,?,?,?,?)", {
	  get<string>(wantAll[n]["onderwerp"]),
	  titel,
	  text,
	  get<int64_t>(wantAll[n]["contentLength"]),
	  id, get<string>(wantAll[n]["datum"]), get<string>(wantAll[n]["category"])  });

      sqlw.addOrReplaceValue({{"uuid", id}, {"contentLength",  get<int64_t>(wantAll[n]["contentLength"])}, {"datum", get<string>(wantAll[n]["datum"])},
		     {"category", get<string>(wantAll[n]["category"])  }}, "indexed");
	    
      indexed++;
    }
  };

  vector<thread> workers;
  for(int n=0; n < 8; ++n)  // number of threads
    workers.emplace_back(worker);
  
  for(auto& w : workers)
    w.join();

  fmt::print("Indexed {} new documents, of which {} were reindexes. {} weren't present, {} of unsupported type, {} were indexed already\n",
	     (int)indexed, reindex.size(), (int)notpresent, (int)wrong, (int)skipped);
}
