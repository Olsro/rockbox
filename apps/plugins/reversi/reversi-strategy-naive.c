/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 * $Id$
 *
 * Copyright (c) 2007 Antoine Cellerier
 *
 * All files in this archive are subject to the GNU General Public License.
 * See the file COPYING in the source tree root for full license agreement.
 *
 * This software is distributed on an "AS IS" basis, WITHOUT WARRANTY OF ANY
 * KIND, either express or implied.
 *
 ****************************************************************************/

#include "reversi-strategy.h"

/**
 * Naive/Stupid strategy:
 *   Random moves
 */

static move_t naive_move_func(const reversi_board_t *game, int player) {
    int num_moves = reversi_count_player_available_moves(game, player);
    int r;
    int row = 0;
    int col = 0;
    if(!num_moves) return MOVE_INVALID;
    r = game->rb->rand()%num_moves;
    while(true) {
        if(reversi_is_valid_move(game, row, col, player)) {
            r--;
            if(r<0) {
                return MAKE_MOVE(row,col,player);
            }
        }
        col ++;
        if(col==BOARD_SIZE) {
            col = 0;
            row ++;
            if(row==BOARD_SIZE) {
                row = 0;
            }
        }
    }
}

const game_strategy_t strategy_naive = {
    true,
    NULL,
    naive_move_func
};
