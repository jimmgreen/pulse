#include "../app/batch_rename_plan.h"
#include <cstdlib>
#define CHECK(x) do { if (!(x)) { std::printf("[FAIL] %s\n", #x); return 1; } } while (0)
namespace pulse::app::rename { extern size_t regex_compile_attempts; }
#include <cstdio>
using namespace pulse::app::rename;
int main() {
 for (int count : {1,1000,10000}) {
  std::vector<Source> sources;
  for(int i=0;i<count;++i) sources.push_back({L"C:\\test\\"+std::to_wstring(i)+L".txt",L"Alpha"+std::to_wstring(i)+L".txt"});
  Step literal; literal.find=L"alpha"; literal.with=L"$x";
  regex_compile_attempts = 0;
  auto plan=Run(sources,{literal},{},{});
  CHECK(!plan.bad_regex && plan.rows[0].new_name==L"$x0.txt");
  CHECK(regex_compile_attempts == 1);
  regex_compile_attempts = 0;
  Step invalid; invalid.regex=true;invalid.find=L"[";
  auto bad=Run(sources,{invalid,literal},{},{});
  CHECK(bad.bad_regex && bad.blocked && bad.rows[0].new_name==plan.rows[0].new_name);
  CHECK(regex_compile_attempts == 2);
  regex_compile_attempts = 0;
  Step regex;regex.regex=true;regex.find=L"(Alpha)";regex.with=L"$1$1";
  Options options;options.order=Order::Name;
  auto multi=Run(sources,{regex,literal},options,{});
  CHECK(multi.rows[0].new_name==L"$x$x0.txt");
  CHECK(regex_compile_attempts == 2);
  std::printf("[PASS] %d files literal dollar, invalid skip, regex capture, multi-step and name order\n",count);
 }
    std::vector<Source> collision{{L"C:\\one\\甲.txt", L"甲.txt"},
        {L"C:\\one\\乙.txt", L"乙.txt"}, {L"C:\\two\\甲.txt", L"甲.txt"}};
    Options collision_options;
    auto collision_plan = RunNames(collision, {L"共同.txt", L"共同.txt", L"共同.txt"}, collision_options, {});
    CHECK(collision_plan.rows[0].new_name == L"共同.txt");
    CHECK(collision_plan.rows[1].new_name != L"共同.txt");
    CHECK(collision_plan.rows[2].new_name == L"共同.txt");
    std::puts("[PASS] Unicode collisions share one parent but remain independent across parents");
}

