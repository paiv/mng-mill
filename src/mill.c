#include <errno.h>
#include <locale.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>


#define MILL_TAPE_SIZE 0x100000
#define MILL_STATES_MAX 1024
#define MILL_STATE_MAX 32
#define MILL_INSTR_MAX 0x10000
#define MILL_STEPS_MAX 1000000


static const char _usage[] =
    "usage: mill -p PROG [-t TAPE] [-o OUT] [-s]\n";

static const char _help_page[] =
    "usage: mill -p PROG [-t TAPE] [-o OUT] [-s]\n"
    "\n"
    "Logic Mill engine https://mng.quest/\n"
    "\n"
    "options:\n"
    "  -h, --help            show this help\n"
    "  -o, --output OUT      output file\n"
    "  -p, --program PROG    program text or file\n"
    "  -s, --steps           log steps taken\n"
    "  -t, --tape TAPE       tape text or file\n"
    ;


struct AppArgs {
    int needs_help;
    int log_steps;
    const char* program;
    const char* tape;
    const char* output;
    FILE* program_file;
    FILE* tape_file;
    FILE* output_file;
};


static void
arg_error(const char* message) {
    fputs(_usage, stderr);
    fprintf(stderr, "error: %s\n", message);
}


static void
arg_perror(const char* message) {
    perror(message);
    fputs(_usage, stderr);
}


static int
parse_args(int argc, const char* argv[], struct AppArgs* args) {
    *args = (struct AppArgs) {};
    int state = 0;

    for (int i = 1; i < argc; ++i) {
        switch (state) {
            case 0:
                if (strcmp(argv[i], "-h") == 0 ||
                    strcmp(argv[i], "--help") == 0) {
                    args->needs_help = 1;
                }
                else if (strcmp(argv[i], "-p") == 0 ||
                    strcmp(argv[i], "--program") == 0) {
                    state = 1;
                }
                else if (strcmp(argv[i], "-s") == 0 ||
                    strcmp(argv[i], "--steps") == 0) {
                    args->log_steps = 1;
                }
                else if (strcmp(argv[i], "-t") == 0 ||
                    strcmp(argv[i], "--tape") == 0) {
                    state = 2;
                }
                else if (strcmp(argv[i], "-o") == 0 ||
                    strcmp(argv[i], "--output") == 0) {
                    state = 3;
                }
                break;

            case 1:
                args->program = argv[i];
                if (strcmp(argv[i], "-") == 0) {
                    args->program_file = stdin;
                }
                state = 0;
                break;

            case 2:
                args->tape = argv[i];
                if (strcmp(argv[i], "-") == 0) {
                    args->tape_file = stdin;
                }
                state = 0;
                break;

            case 3:
                args->output = argv[i];
                if (strcmp(argv[i], "-") == 0) {
                    args->output_file = stdout;
                }
                state = 0;
                break;

            default:
                break;
        }
    }

    if (args->needs_help != 0) {
        return 0;
    }

    if (args->program == NULL) {
        arg_error("-p/--program: expected filename");
        return 1;
    }

    if (args->tape == NULL) {
        if (args->program_file == stdin) {
            arg_error("-t/--tape: expected filename");
            return 1;
        }
        args->tape_file = stdin;
    }

    if (args->program_file != NULL && args->program_file == args->tape_file) {
        arg_error("-t/--tape: conflicting filename");
        return 1;
    }

    if (args->output == NULL) {
        args->output_file = stdout;
    }

    return 0;
}


static int
args_open_file(const char* filename, const char* mode, FILE** file) {
    if (*file != NULL) {
        return 0;
    }

    FILE* fp = fopen(filename, mode);
    if (fp != NULL) {
        *file = fp;
        return 0;
    }

    if (strcmp(mode, "r") != 0) {
        return 1;
    }

    fp = fmemopen((void*) filename, strlen(filename), mode);
    if (fp != NULL) {
        *file = fp;
        return 0;
    }

    return 1;
}


static void
args_close_files(struct AppArgs* args) {
    if (args->output_file != stdout) {
        fclose(args->output_file);
    }

    if (args->program_file != stdin) {
        fclose(args->program_file);
    }

    if (args->tape_file != stdin) {
        fclose(args->tape_file);
    }
}


enum HeadMove {
    HeadMove_none = 0,
    HeadMove_left = 'L',
    HeadMove_right = 'R',
};


struct MillInstr {
    size_t state_in;
    wchar_t char_in;
    size_t state_out;
    wchar_t char_out;
    enum HeadMove move;
};


struct SymTable {
    size_t size;
    wchar_t* symbols[MILL_STATES_MAX + 1];
    size_t datasize;
    wchar_t symdata[MILL_STATES_MAX * (MILL_STATE_MAX + 1)];
};


struct MillProgram {
    struct SymTable symtable;
    size_t syminit;
    size_t symhalt;
    size_t instr_count;
    struct MillInstr instructions[MILL_INSTR_MAX];
};


struct MillTape {
    size_t size;
    size_t pos;
    wchar_t buf[MILL_TAPE_SIZE];
    wchar_t _null;
};


static void
parse_error(const char* fmt, ...) {
    fputs("parse error: ", stderr);
    va_list args;
    va_start(args, fmt);
    vfprintf(stderr, fmt, args);
    va_end(args);
    fputc('\n', stderr);
}


static int
parse_headmove(const wchar_t* text, enum HeadMove* head) {
    size_t n = wcslen(text);
    if (n != 1) {
        parse_error("invalid move instruction %ls", text);
        return 1;
    }
    wchar_t c = text[0];
    switch ((enum HeadMove) c) {
        case HeadMove_left:
        case HeadMove_right:
            *head = c;
            return 0;
        default:
            parse_error("invalid move instruction %ls", text);
            return 1;
    }
}


static int
symtable_insert(struct SymTable* symtable, wchar_t* symbol, size_t* sid) {
    for (size_t i = 0; i < symtable->size; ++i) {
        if (wcscmp(symtable->symbols[i], symbol) == 0) {
            *sid = i;
            return 0;
        }
    }
    if (symtable->size >= MILL_STATES_MAX) {
        parse_error("symbol table limit reached");
        return -1;
    }
    size_t symsize = wcslen(symbol) + 1;
    size_t symmax = sizeof(symtable->symdata) / sizeof(symtable->symdata[0]);
    if (symtable->datasize + symsize > symmax) {
        parse_error("symbols buffer exhausted");
        return -1;
    }
    wchar_t* p = &symtable->symdata[symtable->datasize];
    wcscpy(p, symbol);
    *sid = symtable->size;
    symtable->datasize += symsize;
    symtable->symbols[symtable->size++] = p;
    symtable->symbols[symtable->size] = NULL;
    return 0;
}


static int
mill_parse_instruction(FILE* file, struct SymTable* symtable,
    struct MillInstr* instr) {
    struct MillInstr data = {};
    size_t tokmax = MILL_STATE_MAX;
    wchar_t token[tokmax + 1];
    size_t toksize = 0;
    token[toksize] = L'\0';
    size_t sid;
    int state = 0;

    for (; state < 100; ) {
        wint_t c = fgetwc(file);
        if (c == WEOF) {
            break;
        }

        #if 0
        fprintf(stderr, "** state %d %lc\n", state, c);
        #endif

        switch (state) {
            case 0:
                if (iswspace(c) == 0) {
                    token[toksize++] = c;
                    token[toksize] = L'\0';
                    state = 1;
                }
                break;

            case 1:
                if (iswspace(c) != 0) {
                    int res = symtable_insert(symtable, token, &sid);
                    if (res != 0) { return -1; }
                    toksize = 0;
                    token[toksize] = L'\0';
                    data.state_in = sid;
                    state = 2;
                }
                else if (toksize >= tokmax) {
                    parse_error("symbol is too long: %ls", token);
                    return -1;
                }
                else {
                    token[toksize++] = c;
                    token[toksize] = L'\0';
                    if (toksize == 2 && wcscmp(token, L"//") == 0) {
                        toksize = 0;
                        token[toksize] = L'\0';
                        state = 20;
                    }
                }
                break;

            case 2:
                if (iswspace(c) == 0) {
                    token[toksize++] = c;
                    token[toksize] = L'\0';
                    state = 3;
                }
                break;

            case 3:
                if (iswspace(c) != 0) {
                    data.char_in = token[0];
                    if (data.char_in == L'_') {
                        data.char_in = L'\0';
                    }
                    toksize = 0;
                    token[toksize] = L'\0';
                    state = 4;
                }
                else {
                    parse_error("symbol is too long: %ls", token);
                    return -1;
                }
                break;

            case 4:
                if (iswspace(c) == 0) {
                    token[toksize++] = c;
                    token[toksize] = L'\0';
                    state = 5;
                }
                break;

            case 5:
                if (iswspace(c) != 0) {
                    int res = symtable_insert(symtable, token, &sid);
                    if (res != 0) { return -1; }
                    toksize = 0;
                    token[toksize] = L'\0';
                    data.state_out = sid;
                    state = 6;
                }
                else if (toksize >= tokmax) {
                    parse_error("symbol is too long: %ls", token);
                    return -1;
                }
                else {
                    token[toksize++] = c;
                    token[toksize] = L'\0';
                }
                break;

            case 6:
                if (iswspace(c) == 0) {
                    token[toksize++] = c;
                    token[toksize] = L'\0';
                    state = 7;
                }
                break;

            case 7:
                if (iswspace(c) != 0) {
                    data.char_out = token[0];
                    if (data.char_out == L'_') {
                        data.char_out = L'\0';
                    }
                    toksize = 0;
                    token[toksize] = L'\0';
                    state = 8;
                }
                else {
                    parse_error("symbol is too long: %ls", token);
                    return -1;
                }
                break;

            case 8:
                if (iswspace(c) == 0) {
                    token[toksize++] = c;
                    token[toksize] = L'\0';
                    state = 9;
                }
                break;

            case 9:
                if (iswspace(c) != 0) {
                    int res = parse_headmove(token, &data.move);
                    if (res != 0) { return -1; }
                    toksize = 0;
                    token[toksize] = L'\0';
                    state = c == L'\n' ? 100 : 10;
                }
                else if (toksize >= tokmax) {
                    parse_error("symbol is too long: %ls", token);
                    return -1;
                }
                else {
                    token[toksize++] = c;
                    token[toksize] = L'\0';
                    if (toksize >= 3 && token[toksize-2] == L'/' && token[toksize-1] == L'/') {
                        toksize -= 2;
                        token[toksize] = L'\0';
                        int res = parse_headmove(token, &data.move);
                        if (res != 0) { return -1; }
                        state = 12;
                    }
                }
                break;

            case 10:
                if (c == L'\n') {
                    state = 100;
                }
                else if (c == L'/') {
                    token[toksize++] = c;
                    token[toksize] = L'\0';
                    state = 11;
                }
                else if (iswspace(c) == 0) {
                    parse_error("unexpected token: %lc", c);
                    return -1;
                }
                break;

            case 11:
                if (c == L'/') {
                    state = 12;
                }
                else {
                    token[toksize++] = c;
                    token[toksize] = L'\0';
                    parse_error("unexpected token: %ls", token);
                    return -1;
                }
                break;

            case 12:
                if (c == L'\n') {
                    state = 100;
                }
                break;

            case 20:
                if (c == L'\n') {
                    state = 0;
                }
                break;

            default:
                parse_error("invalid state %d", state);
                return -1;
        }
    }

    #if 0
    fprintf(stderr, "** state %d EOL\n", state);
    #endif

    switch (state) {
        case 0:
        case 20:
            return 0;
        case 1 ... 8:
            parse_error("expecting a token");
            return -1;
        case 9: {
            int res = parse_headmove(token, &data.move);
            if (res != 0) { return -1; }
            *instr = data;
            return 1;
        }
        case 11:
            parse_error("unexpected token: %ls", token);
            return -1;
        case 10:
        case 12:
        case 100:
            *instr = data;
            return 1;
    }
    parse_error("invalid state %d", state);
    return -1;
}


static int
mill_parse_program(FILE* file, struct MillProgram* program) {
    *program = (struct MillProgram) {};
    int res;

    res = symtable_insert(&program->symtable, L"INIT", &program->syminit);
    if (res != 0) { return res; }

    res = symtable_insert(&program->symtable, L"HALT", &program->symhalt);
    if (res != 0) { return res; }

    for (;;) {
        struct MillInstr instr = {};
        int n = mill_parse_instruction(file, &program->symtable, &instr);
        if (n == 0) { break; }
        if (n != 1) { return n; }

        if (program->instr_count >= MILL_INSTR_MAX) {
            parse_error("too many instructions");
            return 1;
        }

        program->instructions[program->instr_count++] = instr;
    }

    return 0;
}


static int
mill_read_tape(FILE* file, struct MillTape* tape) {
    wchar_t* res = fgetws(tape->buf, tape->size, file);
    if (res == NULL) {
        perror("fgets");
        return 1;
    }
    return 0;
}


static int
mill_print_tape(FILE* file, struct MillTape* tape) {
    size_t start = 0;
    size_t end = 0;
    for (size_t i = 0; i < tape->size; ++i) {
        if (tape->buf[start] == L'\0') {
            break;
        }
        else {
            start = (start - 1) % tape->size;
        }
    }
    for (size_t i = 0; i < tape->size; ++i) {
        if (tape->buf[start] != L'\0') {
            break;
        }
        else {
            start = (start + 1) % tape->size;
        }
    }

    int res = fputws(&tape->buf[start], file);
    if (res < 0) {
        perror("fputws");
        return 1;
    }
    if (start > 0 && tape->buf[0] != L'\0') {
        int res = fputws(&tape->buf[0], file);
        if (res < 0) {
            perror("fputws");
            return 1;
        }
    }
    wint_t r = fputwc(L'\n', file);
    if (r == WEOF) {
        perror("fputwc");
        return 1;
    }
    return 0;
}


static int
mill_run(struct MillProgram* prog, struct MillTape* tape,
    size_t* steps) {
    size_t pos = tape->pos;
    size_t state = prog->syminit;
    size_t halt = prog->symhalt;

    for (size_t t = 0; t < MILL_STEPS_MAX; ++t) {
        size_t prev = pos;
        wchar_t c = tape->buf[pos];

        #if 0
        wchar_t* s = prog->symtable.symbols[state];
        fprintf(stderr, "state %ls '%lc'\n", s, c);
        #endif

        for (size_t i = 0; i < prog->instr_count; ++i) {
            struct MillInstr* instr = &prog->instructions[i];
            if (instr->state_in == state && instr->char_in == c) {
                #if 0
                wchar_t* s = prog->symtable.symbols[instr->state_out];
                fprintf(stderr, "  -> %ls '%lc' %c\n", s,
                    instr->char_out, instr->move);
                #endif

                tape->buf[pos] = instr->char_out;
                state = instr->state_out;
                int dp = 0;
                switch (instr->move) {
                    case HeadMove_left: dp = -1; break;
                    case HeadMove_right: dp = 1; break;
                    default:
                        tape->pos = pos;
                        if (steps != NULL) {
                            *steps = t + 1;
                        }
                        fprintf(stderr, "error: invalid head movement\n");
                        return -1;
                }
                pos = (pos + dp) % tape->size;
                if (state == halt) {
                    tape->pos = pos;
                    if (steps != NULL) {
                        *steps = t + 1;
                    }
                    return 0;
                }
                break;
            }
        }
        if (pos == prev) {
            wchar_t* s = prog->symtable.symbols[state];
            if (c == L'\0') {
                c = L'_';
            }
            tape->pos = pos;
            if (steps != NULL) {
                *steps = t + 1;
            }
            fprintf(stderr, "error: unhandled state %ls '%lc'\n", s, c);
            return -1;
        }
    }

    tape->pos = pos;
    if (steps != NULL) {
        *steps = MILL_STEPS_MAX;
    }
    fprintf(stderr, "timed out after %zu instructions\n", (size_t) MILL_STEPS_MAX);
    return 1;
}


static struct MillProgram _Program;
static struct MillTape _Tape;


int main(int argc, const char* argv[]) {
    setlocale(LC_ALL, "");

    struct AppArgs args = {};
    int res = parse_args(argc, argv, &args);
    if (res != 0) { return res; }

    if (args.needs_help) {
        puts(_help_page);
        return 0;
    }
    
    res = args_open_file(args.program, "r", &args.program_file);
    if (res != 0) {
        arg_perror("-p/--program");
        return res;
    }

    res = args_open_file(args.tape, "r", &args.tape_file);
    if (res != 0) {
        arg_perror("-t/--tape");
        return res;
    }

    res = args_open_file(args.output, "w", &args.output_file);
    if (res != 0) {
        arg_perror("-o/--output");
        return res;
    }

    res = mill_parse_program(args.program_file, &_Program);
    if (res != 0) {
        args_close_files(&args);
        return res;
    }

    _Tape.size = sizeof(_Tape.buf) / sizeof(_Tape.buf[0]);

    res = mill_read_tape(args.tape_file, &_Tape);
    if (res != 0) {
        args_close_files(&args);
        return res;
    }

    size_t steps = 0;
    res = mill_run(&_Program, &_Tape, &steps);
    if (res != 0) {
        args_close_files(&args);
        return res;
    }

    if (args.log_steps != 0) {
        fprintf(stderr, "%zu steps\n", steps);
    }

    res = mill_print_tape(args.output_file, &_Tape);
    if (res != 0) {
        args_close_files(&args);
        return res;
    }

    args_close_files(&args);
    return 0;
}
