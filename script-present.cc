
#include <stdlib.h>
#include <curses.h>
#include <signal.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>

#include <string>
#include <vector>

#include <iostream>
#include <fstream>
#include <string>
#include <cstring>

#include <algorithm> 
#include <functional> 
#include <cctype>
#include <locale>

// trim from start
static inline std::string ltrim(std::string s) {
        s.erase(s.begin(), std::find_if(s.begin(), s.end(), std::not1(std::ptr_fun<int, int>(std::isspace))));
        return s;
}

// trim from end
static inline std::string rtrim(std::string s) {
        s.erase(std::find_if(s.rbegin(), s.rend(), std::not1(std::ptr_fun<int, int>(std::isspace))).base(), s.end());
        return s;
}

// trim from both ends
static inline std::string trim(std::string s) {
        return ltrim(rtrim(s));
}

static void finish(int sig);

int row, col;

WINDOW *create_newwin(WINDOW* parent, int height, int width, int starty, int startx) {
	WINDOW *local_win = subwin(parent, height, width, starty, startx);
    return local_win;
}

struct command {
    std::string command, wd, title, output;
    int exitcode;
    std::vector<int> lineposes;
    decltype(COLOR_PAIR(0)) cd_color, body_color;
};

// returns output and exit status
std::tuple<std::string, int> execute_command(const std::string& command) {
    pid_t pid;

    int stdin[2], stdout[2];

    if (pipe(stdin) == -1 || pipe(stdout) == -1) {
        std::cerr << "Failed to create pipe: " << std::strerror(errno) << std::endl;
        std::exit(1);
    }

    if ((pid = fork()) == -1) {
        std::cerr << "Failed to fork: " << std::strerror(errno) << std::endl;
        std::exit(1);
    }

    if (pid == 0) {
        dup2(stdin[0], STDIN_FILENO);
        dup2(stdout[1], STDOUT_FILENO);
        dup2(stdout[1], STDERR_FILENO);

        close(stdin[1]);
        close(stdout[0]);

        char* argv[] = { NULL };
        execvp("/bin/bash", argv);

        std::cerr << "Child process failed to execute bash" << std::endl;
        std::exit(1);
    } else {
        close(stdin[0]);
        close(stdout[1]);
        
        write(stdin[1], command.data(), command.length());
        close(stdin[1]);

        std::string output;

        char buf[1024];
        ssize_t readsize;
        while ((readsize = read(stdout[0], buf, sizeof(buf))) > 0)
            output.append(buf, buf + readsize);

        close(stdout[0]);

        int childstatus;

        do {
            waitpid(pid, &childstatus, 0);
        } while (!WIFEXITED(childstatus));

        return std::make_tuple(output, WEXITSTATUS(childstatus));
    }
}

std::vector<int> find_newlines(const std::string& s) {
    std::vector<int> newlines { 0 };

    for (std::size_t i = 0; i < s.length(); ++i)
        if (s[i] == '\n')
            newlines.push_back(i + 1);

    newlines.push_back(s.length());

    return newlines;
}

bool get_command(std::vector<command>& commands, std::ifstream& infile) {
    std::string line;

    command newcmd;

    {
        char* const wdchar = getcwd(nullptr, 0);
        newcmd.wd = wdchar;
        free(wdchar);
    }

    while (std::getline(infile, line)) {
        if (line.length() == 0)
            continue;

        if (line[0] == '#') {
            if (line.length() == 1 || line[1] == ' ') {
                do {
                    newcmd.output.append(line.substr(1));
                    newcmd.output.push_back('\n');
                } while (std::getline(infile, line) && !line.empty() && line[0] == '#');

                newcmd.command = "";
                newcmd.cd_color = COLOR_PAIR(4);
                newcmd.body_color = COLOR_PAIR(7);
                newcmd.exitcode = 0;
                newcmd.lineposes = find_newlines(newcmd.output);

                if (!commands.empty() && newcmd.title.empty())
                    newcmd.title = commands.back().title;

                commands.push_back(std::move(newcmd));
                return true;
            } else if (line.length() > 1) {
                if (line[1] == '!') // ignore hashbang
                    continue;
                else if (line[1] == '#')
                    newcmd.title = trim(line.substr(2));
                else if (line[1] == ':')
                    newcmd.wd = trim(line.substr(2));
            }

            continue;
        }

        newcmd.command = line;
        newcmd.cd_color = COLOR_PAIR(4);
    
        if (chdir(newcmd.wd.data()))
            newcmd.cd_color = COLOR_PAIR(1);

        newcmd.body_color = COLOR_PAIR(7);

        std::tie(newcmd.output, newcmd.exitcode) = execute_command(newcmd.command);

        if (newcmd.exitcode != 0)
            newcmd.body_color = COLOR_PAIR(1);

        newcmd.lineposes = find_newlines(newcmd.output);

        if (!commands.empty() && newcmd.title.empty())
            newcmd.title = commands.back().title;

        commands.push_back(std::move(newcmd));

        return true;
    }

    return false;
}

enum scroll_dir {
    IGNORE, UP_ONE, DOWN_ONE, ARBITRARY
};

void show_lines(WINDOW* bodywin, const std::string& output, const std::vector<int>& lineposes, int begin, int end, scroll_dir dir) {
    int bodyrow, bodycol, beginpos, endpos, x = 0, y = 0;
    getmaxyx(bodywin, bodyrow, bodycol);

    end = std::min(end, (int)lineposes.size() - 1);

    switch (dir) {
    case IGNORE:
        return;
        
    case ARBITRARY:
        wclear(bodywin);
        beginpos = lineposes.at(begin);
        endpos = lineposes.at(end) - 1;
        break;

    case UP_ONE:
        wscrl(bodywin, -1);
        beginpos = lineposes.at(begin);
        endpos = lineposes.at(begin + 1) - 1;
        break;

    case DOWN_ONE:
        wscrl(bodywin, 1);
        beginpos = lineposes.at(end - 1);
        endpos = lineposes.at(end) - 1;
        y = bodyrow - 1;
        break;
    }

    mvwaddnstr(bodywin, y, x, output.data() + beginpos, endpos - beginpos);
    wrefresh(bodywin);
}

void show_command(const command& cmd, WINDOW* titlewin, WINDOW* bodywin) {
    int bodyrow, bodycol;
    getmaxyx(bodywin, bodyrow, bodycol);

    wclear(titlewin);
    wattron(titlewin, cmd.body_color);
    mvwaddnstr(titlewin, 2, 1, cmd.command.c_str(), cmd.command.length());
    box(titlewin, 0, 0);
    wattron(titlewin, COLOR_PAIR(2));
    mvwaddnstr(titlewin, 1, bodycol / 2 - cmd.title.length() / 2, cmd.title.c_str(), cmd.title.length());

    wattron(titlewin, cmd.cd_color);
    mvwaddnstr(titlewin, 2, col - cmd.wd.length() - 1, cmd.wd.c_str(), cmd.wd.length());

    wclear(bodywin);
    wrefresh(bodywin);
    wrefresh(titlewin);

    wattron(bodywin, cmd.body_color);

    show_lines(bodywin, cmd.output, cmd.lineposes, 0, bodyrow, ARBITRARY);
    wrefresh(bodywin);
}

int main(const int argc, const char* const argv[])
{
    if (argc < 2) {
        std::cout << "Usage: " << *argv << " SCRIPT" << std::endl;
        return 1;
    }

    std::ifstream infile(argv[1]);

    signal(SIGINT, finish);

    setlocale(LC_CTYPE, "");
    auto mainwin = initscr();      /* initialize the curses library */
    keypad(stdscr, TRUE);  /* enable keyboard mapping */
    nonl();         /* tell curses not to do NL->CR/NL on output */
    cbreak();       /* take input chars one at a time, no wait for \n */
    noecho();

    if (has_colors()) {
        start_color();

        init_pair(1, COLOR_RED,     COLOR_BLACK);
        init_pair(2, COLOR_GREEN,   COLOR_BLACK);
        init_pair(3, COLOR_YELLOW,  COLOR_BLACK);
        init_pair(4, COLOR_BLUE,    COLOR_BLACK);
        init_pair(5, COLOR_CYAN,    COLOR_BLACK);
        init_pair(6, COLOR_MAGENTA, COLOR_BLACK);
        init_pair(7, COLOR_WHITE,   COLOR_BLACK);
    }

    getmaxyx(stdscr, row, col);

    auto titlewin = create_newwin(mainwin, 4, col, 0, 0),
         bodywin = create_newwin(mainwin, row - 4, col - 1, 4, 1);

    int bodyrow, bodycol;
    getmaxyx(bodywin, bodyrow, bodycol);

    scrollok(bodywin, true);
    idlok(bodywin, true);

    std::vector<command> commands;

    int cmd_idx = 0;

    if (!get_command(commands, infile)) {
        std::cout << "Empty presentation" << std::endl;
        return 1;
    }

    while (true) {
        if (cmd_idx < 0)
            cmd_idx = 0;
        
        if (cmd_idx >= commands.size())
            if (!get_command(commands, infile))
                cmd_idx = commands.size() - 1;

        command& cmd = commands.at(cmd_idx);

        show_command(cmd, titlewin, bodywin);

        int current_line = 0, ch;
        while ((ch = getch()) != 'q') {
            scroll_dir dir = IGNORE;

            switch (ch) {
            case '\n':
            case '\r':
            case ' ':
            case KEY_ENTER:
                ++cmd_idx;
                goto break_keyboard_loop;
                break;
            case KEY_BACKSPACE:
                --cmd_idx;
                goto break_keyboard_loop;
                break;
            case KEY_HOME:
                dir = ARBITRARY;
                current_line = 0;
                break;
            case KEY_END:
                dir = ARBITRARY;
                if (cmd.lineposes.size() > bodyrow)
                    current_line = cmd.lineposes.size() - bodyrow - 1;
                else
                    current_line = 0;
                break;
            case KEY_UP:
                if (current_line - 1 >= 0) {
                    --current_line;
                    dir = UP_ONE;
                }

                break;

            case KEY_DOWN:
                if (current_line + 1 + bodyrow < cmd.lineposes.size()) {
                    ++current_line;
                    dir = DOWN_ONE;
                }

                break;

            case KEY_LEFT:
                break;

            case KEY_RIGHT:
                break;
            }

            show_lines(bodywin, cmd.output, cmd.lineposes, current_line, current_line + bodyrow, dir);
        }

        break_keyboard_loop:;
    }

    finish(0);
}

static void finish(int sig)
{
    endwin();

    exit(0);
}
