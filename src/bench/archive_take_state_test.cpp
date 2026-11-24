#include "../ui/seven_zip_take_state.h"
#include <cstdio>
#include <initializer_list>
using namespace pulse::ui;
int main() {
 int checks=0,failures=0;
 auto check=[&](bool ok,const char* name){++checks;failures+=!ok;std::printf("[%s] %s\n",ok?"PASS":"FAIL",name);};
 check(DecodeSevenZipTakeOwner('p')==SevenZipTakeOwner::Pulse,"Pulse is confirmed");
 check(DecodeSevenZipTakeOwner('x')==SevenZipTakeOwner::Off,"excluded remains off");
 check(DecodeSevenZipTakeOwner('?')==SevenZipTakeOwner::Unknown,"unknown protocol state never means completed");
 check(SevenZipNeedsConfirmation(DecodeSevenZipTakeOwner('f')),"no effective app still requires completion");
 check(SevenZipNeedsConfirmation(DecodeSevenZipTakeOwner('u')),"query failure remains pending");
 check(SevenZipNeedsConfirmation(DecodeSevenZipTakeOwner('o')),"existing owner requires confirmation");
 check(!SevenZipNeedsConfirmation(DecodeSevenZipTakeOwner('x'))&&!SevenZipNeedsConfirmation(DecodeSevenZipTakeOwner('p')),"off and Pulse do not prompt");
 int ours=0,pending=0;for(char state: {'p','p','p','p','f','f','f','f','f','f','f','f'}){auto o=DecodeSevenZipTakeOwner(state);ours+=o==SevenZipTakeOwner::Pulse;pending+=SevenZipNeedsConfirmation(o);}
 check(ours==4 && pending==8,"screenshot 4/12 is pending eight, never all done");
 std::printf("ARCHIVE_TAKE_STATE checks=%d failures=%d\n",checks,failures);return failures?1:0;
}

