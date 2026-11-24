#include "../host/model/desktop_model.h"
#include <cstdio>
#include <vector>
using namespace pulse::desktop::model;
int main() {
    int failed = 0;
    auto check = [&](bool value, const char* name) {
        std::printf("[%s] %s\n", value ? "PASS" : "FAIL", name);
        if (!value) ++failed;
    };
    const int positions[][4] = {{2,4,2,1},{0,3,2,2},{0,1,1,1},{1,5,4,1},
        {0,2,4,1},{1,1,4,1},{1,0,4,1},{0,6,4,1}};
    std::vector<DesktopTile> tiles;
    for (int i = 0; i < 8; ++i) {
        DesktopTile t{};
        t.id = std::to_string(i);
        t.col = positions[i][0]; t.row = positions[i][1];
        t.size = {positions[i][2], positions[i][3]};
        tiles.push_back(t);
    }
    const auto result = PreviewTilePlacement(tiles, "0", 0, 2, {2, 2});
    check(result.valid, "backtrack after nearest position fragments available space");
    bool unchanged = tiles[0].col == 2 && tiles[0].row == 4;
    bool valid = result.valid;
    bool fixed = result.valid;
    if (result.valid) {
        for (size_t i = 0; i < tiles.size(); ++i) {
            const auto& a = result.updated_tiles[i];
            valid &= a.col >= 0 && a.row >= 0 && a.col + a.size.cols <= kTileCols && a.row + a.size.rows <= kTileRows;
            for (size_t j = 0; j < i; ++j) {
                const auto& b = result.updated_tiles[j];
                valid &= !RectI{a.col,a.row,a.size.cols,a.size.rows}.intersects({b.col,b.row,b.size.cols,b.size.rows});
            }
            if (i != 0 && i != 1 && i != 4) fixed &= a.col == tiles[i].col && a.row == tiles[i].row;
        }
    }
    check(unchanged, "preview does not mutate source model");
    check(valid, "placement stays bounded and non-overlapping");
    check(fixed, "unaffected tiles keep their positions");
    check(!PreviewTilePlacement(tiles,"0",4,6,{2,2}).valid, "reject out-of-bounds target");
    const auto defaults = MakeDefaultTiles();
    check(!PreviewTilePlacement(defaults,"downloads",0,2,{4,4}).valid, "reject impossible oversized layout");
    DesktopTile inserted{}; inserted.id = "extra"; inserted.col=4; inserted.row=6; inserted.size={1,1};
    check(InsertNewTile(defaults,inserted).valid, "insert keeps active index before moving vector");
#ifndef BASELINE_GEOMETRY
    bool geometry = true;
    for (const RectI board : {RectI{1360,44,532,748}, RectI{100,30,319,449}, RectI{10,10,266,374}}) {
        for (const TileSize size : AllowedTileSizes()) {
            for (int r=0;r<=kTileRows-size.rows;++r) for(int c=0;c<=kTileCols-size.cols;++c) {
                DesktopTile t{};t.col=c;t.row=r;t.size=size;
                const auto rect=ComputeTilePixelRect(board,t);
                const auto cell=ComputeTileDropCell(board,size,{rect.x,rect.y});
                geometry &= cell.x==c && cell.y==r;
                const auto slight=ComputeTileDropCell(board,size,{rect.x+6,rect.y+6});
                geometry &= slight.x==c && slight.y==r;
            }
        }
    }
    check(geometry,"scaled geometry roundtrip and drag-threshold stability");
#endif
    return failed ? 1 : 0;
}
