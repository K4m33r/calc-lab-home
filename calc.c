// Kamer Akçin 241ADB155
// Repository: https://github.com/K4m33r/calc-lab-home
// Compile with: gcc -O2 -Wall -Wextra -std=c17 -o calc calc.c -lm
// Resources used: RTU "Graded lab: Arithmetic Parser in C" instructions and
//                 calc-tests.zip; OpenAI ChatGPT (GPT-6) for program design,
//                 code drafting, test review, and explanation.
// AI conversation: https://chatgpt.com/share/6ade94c0-ccfc-83eb-a5eb-cee28fe3aee6
// Original user prompt: "sana hocanın istediğini atacağım nasıl bunu seninle
// yapıp puan alacağız onu bulmak senin görevin"; followed by the complete
// RTU assignment text. The AI comments below quote relevant parts of that
// assignment as prompt context; they are not separate invented user prompts.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <math.h>
#include <errno.h>
#include <dirent.h>
#include <sys/stat.h>

/* ASCII lowercase name parts make portable filenames; keep the student ID exact. */
#define STUDENT_FIRST "kamer"
#define STUDENT_LAST  "akcin"
#define STUDENT_ID    "241ADB155"

/*
 * AI (ChatGPT), prompt context: original request and RTU rule "If either
 * operand is a float, the operation is done in double" plus the integer
 * division rule. A type flag preserves integer arithmetic when both operands
 * are integers; double is used only when a floating operand is involved.
 */
typedef struct {
    int is_float;
    long long integer;
    double real;
} Value;

typedef struct {
    const char *line;
    size_t length;
    size_t cursor;
    size_t line_start;  /* First character's 1-based position within the file. */
    size_t error_pos;
    int fatal_error;
} Parser;

typedef struct {
    Value left;
    size_t operator_at;
    size_t exponent_at;
} PowerStep;

static int expression(Parser *p, Value *out);

static void spaces(Parser *p)
{
    while (p->cursor < p->length &&
           (p->line[p->cursor] == ' ' || p->line[p->cursor] == '\t')) {
        p->cursor++;
    }
}

/*
 * AI (ChatGPT), prompt context: original user request quoted in the header,
 * followed by the RTU rule "Positions are 1-based and count every character
 * in the file, including each \n and the characters in comment lines."
 * I used the suggestion to keep the absolute start of each line. Adding the
 * local zero-based column yields a 1-based file position; even ignored lines
 * advance line_start, and a missing final token points to EOL or EOF+1.
 */
static int syntax_error(Parser *p, size_t column)
{
    if (p->error_pos == 0) {
        p->error_pos = p->line_start + column;
    }
    return 0;
}

static Value integer_value(long long number)
{
    Value v;
    v.is_float = 0;
    v.integer = number;
    v.real = 0.0;
    return v;
}

static Value float_value(double number)
{
    Value v;
    v.is_float = 1;
    v.integer = 0;
    v.real = number;
    return v;
}

static double as_double(Value v)
{
    return v.is_float ? v.real : (double)v.integer;
}

/*
 * AI (ChatGPT), prompt context: original request and the RTU rules "There
 * are no unary operators" and "Floats ... digits, '.', digits". I validate
 * the token before conversion because strtod accepts forms like .5 or 5.
 * that are forbidden by this assignment.
 */
static int primary(Parser *p, Value *out)
{
    size_t start;
    spaces(p);
    start = p->cursor;
    if (start == p->length) {
        return syntax_error(p, start);
    }

    if (p->line[p->cursor] == '(') {
        p->cursor++;
        if (!expression(p, out)) {
            return 0;
        }
        spaces(p);
        /* A missing ')' points to the unconsumed token, including end-of-line. */
        if (p->cursor >= p->length || p->line[p->cursor] != ')') {
            return syntax_error(p, p->cursor);
        }
        p->cursor++;
        return 1;
    }

    if (!isdigit((unsigned char)p->line[p->cursor])) {
        /* No unary +/-; invalid characters and unexpected ')' point here. */
        return syntax_error(p, p->cursor);
    }

    while (p->cursor < p->length &&
           isdigit((unsigned char)p->line[p->cursor])) {
        p->cursor++;
    }

    if (p->cursor < p->length && p->line[p->cursor] == '.') {
        size_t decimal_point = p->cursor++;
        /* A dot must have digits on both sides: "5." and ".5" are invalid. */
        if (p->cursor == p->length ||
            !isdigit((unsigned char)p->line[p->cursor])) {
            return syntax_error(p, decimal_point);
        }
        while (p->cursor < p->length &&
               isdigit((unsigned char)p->line[p->cursor])) {
            p->cursor++;
        }
        /* Lexical validation above prevents strtod's wider syntax being accepted. */
        *out = float_value(strtod(p->line + start, NULL));
        return 1;
    }

    /* The task guarantees integer literals do not overflow long long. */
    *out = integer_value(strtoll(p->line + start, NULL, 10));
    return 1;
}

static long long integer_power(long long base, long long exponent)
{
    long long result = 1;
    /* Exponentiation by squaring keeps even very large exponents fast. */
    while (exponent > 0) {
        if (exponent % 2 != 0) {
            result *= base;
        }
        exponent /= 2;
        if (exponent != 0) {
            base *= base;
        }
    }
    return result;
}

/*
 * AI (ChatGPT), prompt context: original request and RTU rule "`**`,
 * right-associative: 2 ** 3 ** 2 == 2 ** 9 == 512". I used the proposed
 * right-to-left reduction of a chain of powers. Unlike a recursive call for
 * each '**', this also avoids deep recursion on a long unparenthesized chain.
 */
static int power(Parser *p, Value *out)
{
    Value right;
    PowerStep *steps = NULL;
    size_t used = 0, capacity = 0, i;
    int ok = 0;

    if (!primary(p, &right)) {
        return 0;
    }

    while (1) {
        size_t operator_at, exponent_at;
        PowerStep *new_steps;
        spaces(p);
        if (p->cursor + 1 >= p->length ||
            p->line[p->cursor] != '*' || p->line[p->cursor + 1] != '*') {
            break;
        }

        operator_at = p->cursor;
        p->cursor += 2;
        spaces(p);
        exponent_at = p->cursor;

        if (used == capacity) {
            size_t next_capacity = capacity ? capacity * 2 : 8;
            if (next_capacity < capacity ||
                next_capacity > ((size_t)-1) / sizeof(*steps)) {
                p->fatal_error = 1;
                goto done;
            }
            new_steps = realloc(steps, next_capacity * sizeof(*steps));
            if (new_steps == NULL) {
                p->fatal_error = 1;
                goto done;
            }
            steps = new_steps;
            capacity = next_capacity;
        }
        steps[used].left = right;
        steps[used].operator_at = operator_at;
        steps[used].exponent_at = exponent_at;
        used++;
        if (!primary(p, &right)) {
            goto done;
        }
    }

    /* A negative integer exponent is checked on reduction, at its first token.
       Floating non-finite results instead point at the '**' operator. */
    for (i = used; i > 0; i--) {
        PowerStep step = steps[i - 1];
        if (!step.left.is_float && !right.is_float) {
            if (right.integer < 0) {
                syntax_error(p, step.exponent_at);
                goto done;
            }
            right = integer_value(integer_power(step.left.integer, right.integer));
        } else {
            double answer = pow(as_double(step.left), as_double(right));
            if (!isfinite(answer)) {
                syntax_error(p, step.operator_at);
                goto done;
            }
            right = float_value(answer);
        }
    }

    *out = right;
    ok = 1;
done:
    free(steps);
    return ok;
}

/*
 * AI (ChatGPT), prompt context: original request and the RTU grammar
 * "expr := term ...; term := power ...; power := primary ...".
 * I used separate parsing levels so '*' and '/' complete before '+' and '-'.
 * Their loops evaluate left-to-right, while power() reduces right-to-left.
 * This structure implements the required precedence without extra passes.
 */
static int term(Parser *p, Value *out)
{
    Value left;
    if (!power(p, &left)) {
        return 0;
    }

    while (1) {
        char op;
        size_t operator_at, operand_at;
        Value right;
        spaces(p);
        if (p->cursor == p->length ||
            (p->line[p->cursor] != '*' && p->line[p->cursor] != '/') ||
            (p->line[p->cursor] == '*' && p->cursor + 1 < p->length &&
             p->line[p->cursor + 1] == '*')) {
            break;
        }
        operator_at = p->cursor;
        op = p->line[p->cursor++];
        spaces(p);
        operand_at = p->cursor;
        if (!power(p, &right)) {
            return 0;
        }

        if (op == '/' &&
            ((!right.is_float && right.integer == 0) ||
             (right.is_float && right.real == 0.0))) {
            /* Division by zero points to the start of its right operand. */
            return syntax_error(p, operand_at);
        }
        if (left.is_float || right.is_float) {
            double a = as_double(left), b = as_double(right);
            double answer = (op == '*') ? a * b : a / b;
            if (!isfinite(answer)) {
                return syntax_error(p, operator_at);
            }
            left = float_value(answer);
        } else {
            left = integer_value((op == '*')
                ? left.integer * right.integer
                : left.integer / right.integer);
        }
    }
    *out = left;
    return 1;
}

static int expression(Parser *p, Value *out)
{
    Value left;
    if (!term(p, &left)) {
        return 0;
    }
    while (1) {
        char op;
        size_t operator_at;
        Value right;
        spaces(p);
        if (p->cursor == p->length ||
            (p->line[p->cursor] != '+' && p->line[p->cursor] != '-')) {
            break;
        }
        operator_at = p->cursor;
        op = p->line[p->cursor++];
        if (!term(p, &right)) {
            return 0;
        }
        /* A floating operand promotes the entire operation to double.
           %.15g at output deliberately does not append a trailing '.0'. */
        if (left.is_float || right.is_float) {
            double a = as_double(left), b = as_double(right);
            double answer = (op == '+') ? a + b : a - b;
            if (!isfinite(answer)) {
                return syntax_error(p, operator_at);
            }
            left = float_value(answer);
        } else {
            left = integer_value((op == '+')
                ? left.integer + right.integer
                : left.integer - right.integer);
        }
    }
    *out = left;
    return 1;
}

/* Returns 1=expression, 0=comment/blank, -1=syntax, -2=resource failure. */
static int parse_line(const char *line, size_t length, size_t line_start,
                      Value *out, size_t *error_pos)
{
    Parser p;
    p.line = line;
    p.length = length;
    p.cursor = 0;
    p.line_start = line_start;
    p.error_pos = 0;
    p.fatal_error = 0;

    spaces(&p);
    /* A hash starts a comment only when it is the first non-blank character. */
    if (p.cursor == p.length || p.line[p.cursor] == '#') {
        return 0;
    }
    if (!expression(&p, out)) {
        if (p.fatal_error) {
            return -2; /* A resource failure is not an arithmetic error. */
        }
        *error_pos = p.error_pos;
        return -1;
    }
    spaces(&p);
    if (p.cursor != p.length) {
        syntax_error(&p, p.cursor);
        *error_pos = p.error_pos;
        return -1;
    }
    return 1;
}

/*
 * AI (ChatGPT), prompt context: original request and RTU rule "There is no
 * limit on file size or line length: the tests include files of several
 * megabytes". I used a doubling realloc buffer for one line at a time,
 * avoiding repeated copies for each character and storing no entire file.
 * Reading as bytes also lets '\n' contribute exactly one file position.
 * Return 1=line, 0=EOF, -1=read or memory failure.
 */
static int read_line(FILE *input, char **buffer, size_t *capacity,
                     size_t *length, int *has_newline)
{
    int c;
    *length = 0;
    *has_newline = 0;

    c = fgetc(input);
    if (c == EOF) {
        return ferror(input) ? -1 : 0;
    }
    while (c != EOF) {
        char *grown;
        size_t next_capacity;
        if (c == '\n') {
            *has_newline = 1;
            break;
        }
        if (*length + 1 >= *capacity) {
            next_capacity = *capacity ? *capacity * 2 : 256;
            if (next_capacity <= *capacity) {
                return -1;
            }
            grown = realloc(*buffer, next_capacity);
            if (grown == NULL) {
                return -1;
            }
            *buffer = grown;
            *capacity = next_capacity;
        }
        (*buffer)[(*length)++] = (char)c;
        c = fgetc(input);
    }
    if (c == EOF && ferror(input)) {
        return -1;
    }
    if (*length + 1 >= *capacity) {
        char *grown = realloc(*buffer, *length + 2);
        if (grown == NULL) {
            return -1;
        }
        *buffer = grown;
        *capacity = *length + 2;
    }
    (*buffer)[*length] = '\0';
    return 1;
}

static char *copy_string(const char *s)
{
    size_t length = strlen(s) + 1;
    char *copy = malloc(length);
    if (copy) {
        memcpy(copy, s, length);
    }
    return copy;
}

static char *join_path(const char *folder, const char *filename)
{
    size_t a = strlen(folder), b = strlen(filename);
    int slash = (a != 0 && folder[a - 1] != '/');
    char *joined = malloc(a + b + (size_t)slash + 1);
    if (joined != NULL) {
        memcpy(joined, folder, a);
        if (slash) {
            joined[a++] = '/';
        }
        memcpy(joined + a, filename, b + 1);
    }
    return joined;
}

/* Last component, ignoring trailing slashes for directory arguments. */
static char *file_stem(const char *path, int is_directory)
{
    size_t end = strlen(path), start, len;
    char *stem;
    if (is_directory) {
        while (end > 1 && path[end - 1] == '/') {
            end--;
        }
    }
    start = end;
    while (start > 0 && path[start - 1] != '/') {
        start--;
    }
    len = end - start;
    if (!is_directory && len >= 4 &&
        memcmp(path + end - 4, ".txt", 4) == 0) {
        len -= 4;
    }
    stem = malloc(len + 1);
    if (stem != NULL) {
        memcpy(stem, path + start, len);
        stem[len] = '\0';
    }
    return stem;
}

static char *make_output_folder(const char *input, int is_directory)
{
    char *stem = file_stem(input, is_directory);
    char *folder;
    size_t size;
    if (stem == NULL) {
        return NULL;
    }
    size = strlen(stem) + strlen(STUDENT_FIRST) + strlen(STUDENT_ID) + 3;
    folder = malloc(size);
    if (folder != NULL) {
        snprintf(folder, size, "%s_%s_%s", stem, STUDENT_FIRST, STUDENT_ID);
    }
    free(stem);
    return folder;
}

static char *make_result_name(const char *input)
{
    char *stem = file_stem(input, 0);
    char *filename;
    size_t size;
    if (stem == NULL) {
        return NULL;
    }
    size = strlen(stem) + strlen(STUDENT_FIRST) + strlen(STUDENT_LAST)
         + strlen(STUDENT_ID) + strlen("___.txt") + 1;
    filename = malloc(size);
    if (filename != NULL) {
        snprintf(filename, size, "%s_%s_%s_%s.txt", stem,
                 STUDENT_FIRST, STUDENT_LAST, STUDENT_ID);
    }
    free(stem);
    return filename;
}

static int ensure_directory(const char *folder)
{
    struct stat st;
    if (mkdir(folder, 0777) != 0 && errno != EEXIST) {
        fprintf(stderr, "calc: cannot create output directory '%s': %s\n",
                folder, strerror(errno));
        return 0;
    }
    if (stat(folder, &st) != 0 || !S_ISDIR(st.st_mode)) {
        fprintf(stderr, "calc: invalid output directory '%s'\n", folder);
        return 0;
    }
    return 1;
}

/*
 * AI (ChatGPT), prompt context: original request and RTU rule "If any error
 * occurs anywhere in the file, the output file contains only ERROR:<pos>
 * for the first error, with no partial results." I used tmpfile() to store
 * intermediate results, copying them to the output only if all expressions
 * succeed. A single failure therefore discards every earlier result.
 */
static int process_file(const char *input_path, const char *output_folder)
{
    FILE *input = NULL, *temporary = NULL, *output = NULL;
    char *line = NULL, *result_name = NULL, *output_path = NULL;
    size_t capacity = 0, length, preceding_bytes = 0, error_pos = 0;
    int has_newline, read_status, status, success = 0;
    char block[16384];
    size_t bytes;

    input = fopen(input_path, "rb");
    if (input == NULL) {
        fprintf(stderr, "calc: cannot open input '%s': %s\n",
                input_path, strerror(errno));
        goto done;
    }
    temporary = tmpfile();
    if (temporary == NULL) {
        fprintf(stderr, "calc: cannot make temporary output: %s\n", strerror(errno));
        goto done;
    }

    while ((read_status = read_line(input, &line, &capacity,
                                    &length, &has_newline)) == 1) {
        Value value;
        status = parse_line(line, length, preceding_bytes + 1,
                            &value, &error_pos);
        if (status == -2) {
            fprintf(stderr, "calc: out of memory while parsing '%s'\n", input_path);
            goto done;
        }
        if (status < 0) {
            break;
        }
        if (status > 0) {
            if (value.is_float) {
                if (fprintf(temporary, "%.15g\n", value.real) < 0) {
                    fprintf(stderr, "calc: cannot write temporary result\n");
                    goto done;
                }
            } else if (fprintf(temporary, "%lld\n", value.integer) < 0) {
                fprintf(stderr, "calc: cannot write temporary result\n");
                goto done;
            }
        }
        preceding_bytes += length + (size_t)has_newline;
    }
    if (read_status < 0) {
        fprintf(stderr, "calc: failed to read '%s' or ran out of memory\n", input_path);
        goto done;
    }

    result_name = make_result_name(input_path);
    if (result_name != NULL) {
        output_path = join_path(output_folder, result_name);
    }
    if (output_path == NULL) {
        fprintf(stderr, "calc: out of memory building output filename\n");
        goto done;
    }
    output = fopen(output_path, "wb");
    if (output == NULL) {
        fprintf(stderr, "calc: cannot open output '%s': %s\n",
                output_path, strerror(errno));
        goto done;
    }
    if (error_pos != 0) {
        if (fprintf(output, "ERROR:%zu\n", error_pos) < 0) {
            fprintf(stderr, "calc: cannot write error result\n");
            goto done;
        }
    } else {
        if (fseek(temporary, 0L, SEEK_SET) != 0) {
            fprintf(stderr, "calc: cannot rewind temporary results\n");
            goto done;
        }
        while ((bytes = fread(block, 1, sizeof(block), temporary)) > 0) {
            if (fwrite(block, 1, bytes, output) != bytes) {
                fprintf(stderr, "calc: cannot write '%s'\n", output_path);
                goto done;
            }
        }
        if (ferror(temporary)) {
            fprintf(stderr, "calc: cannot read temporary results\n");
            goto done;
        }
    }
    if (fclose(output) != 0) {
        output = NULL;
        fprintf(stderr, "calc: cannot close output '%s'\n", output_path);
        goto done;
    }
    output = NULL;
    success = 1;

done:
    if (output != NULL) {
        fclose(output);
    }
    if (temporary != NULL) {
        fclose(temporary);
    }
    if (input != NULL) {
        fclose(input);
    }
    free(line);
    free(result_name);
    free(output_path);
    return success;
}

static int is_txt(const char *name)
{
    size_t len = strlen(name);
    return len >= 4 && strcmp(name + len - 4, ".txt") == 0;
}

static int compare_names(const void *a, const void *b)
{
    const char *const *name_a = a;
    const char *const *name_b = b;
    return strcmp(*name_a, *name_b);
}

static int process_directory(const char *input_folder, const char *output_folder)
{
    DIR *dir = opendir(input_folder);
    struct dirent *entry;
    char **names = NULL;
    size_t used = 0, capacity = 0, i;
    int success = 0;

    if (dir == NULL) {
        fprintf(stderr, "calc: cannot open directory '%s': %s\n",
                input_folder, strerror(errno));
        return 0;
    }

    /* Collect the initial contents before producing any outputs: a -d and -o
       referring to the same directory cannot consume freshly created files. */
    errno = 0;
    while ((entry = readdir(dir)) != NULL) {
        char *full_path;
        struct stat st;
        if (!is_txt(entry->d_name)) {
            errno = 0;
            continue;
        }
        full_path = join_path(input_folder, entry->d_name);
        if (full_path == NULL) {
            fprintf(stderr, "calc: out of memory\n");
            goto done;
        }
        if (stat(full_path, &st) == 0 && S_ISREG(st.st_mode)) {
            char *new_name;
            if (used == capacity) {
                size_t next_capacity = capacity ? capacity * 2 : 16;
                char **expanded;
                if (next_capacity < capacity ||
                    next_capacity > ((size_t)-1) / sizeof(*names)) {
                    fprintf(stderr, "calc: too many files\n");
                    free(full_path);
                    goto done;
                }
                expanded = realloc(names, next_capacity * sizeof(*names));
                if (expanded == NULL) {
                    fprintf(stderr, "calc: out of memory\n");
                    free(full_path);
                    goto done;
                }
                names = expanded;
                capacity = next_capacity;
            }
            new_name = copy_string(entry->d_name);
            if (new_name == NULL) {
                fprintf(stderr, "calc: out of memory\n");
                free(full_path);
                goto done;
            }
            names[used++] = new_name;
        }
        free(full_path);
        errno = 0;
    }
    if (errno != 0) {
        fprintf(stderr, "calc: error reading directory '%s': %s\n",
                input_folder, strerror(errno));
        goto done;
    }
    closedir(dir);
    dir = NULL;
    qsort(names, used, sizeof(*names), compare_names);
    if (!ensure_directory(output_folder)) {
        goto done;
    }
    for (i = 0; i < used; i++) {
        char *path = join_path(input_folder, names[i]);
        int ok;
        if (path == NULL) {
            fprintf(stderr, "calc: out of memory\n");
            goto done;
        }
        ok = process_file(path, output_folder);
        free(path);
        if (!ok) {
            goto done;
        }
    }
    success = 1;

done:
    if (dir != NULL) {
        closedir(dir);
    }
    for (i = 0; i < used; i++) {
        free(names[i]);
    }
    free(names);
    return success;
}

static int usage(void)
{
    fprintf(stderr,
            "Usage: calc [-d DIR | --dir DIR] [-o OUTDIR | --output-dir OUTDIR] [input.txt]\n");
    return 1;
}

/*
 * AI (ChatGPT), prompt context: original request and the RTU command syntax
 * "calc [-d DIR | --dir DIR] [-o OUTDIR | --output-dir OUTDIR] [input.txt]".
 * I used argument validation to distinguish one file from directory mode;
 * invalid options are operational errors, not expression ERROR:<pos> results.
 */
int main(int argc, char **argv)
{
    const char *input_path = NULL, *input_folder = NULL, *output_arg = NULL;
    const char *output_folder;
    char *default_output = NULL;
    int i, success;

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-d") == 0 || strcmp(argv[i], "--dir") == 0) {
            if (input_folder != NULL || ++i >= argc || argv[i][0] == '-') {
                return usage();
            }
            input_folder = argv[i];
        } else if (strcmp(argv[i], "-o") == 0 ||
                   strcmp(argv[i], "--output-dir") == 0) {
            if (output_arg != NULL || ++i >= argc || argv[i][0] == '-') {
                return usage();
            }
            output_arg = argv[i];
        } else if (argv[i][0] == '-' || input_path != NULL) {
            return usage();
        } else {
            input_path = argv[i];
        }
    }
    if ((input_path == NULL) == (input_folder == NULL)) {
        return usage();
    }

    if (output_arg == NULL) {
        default_output = make_output_folder(
            input_folder ? input_folder : input_path, input_folder != NULL);
        if (default_output == NULL) {
            fprintf(stderr, "calc: out of memory\n");
            return 1;
        }
    }
    output_folder = output_arg ? output_arg : default_output;
    if (input_folder != NULL) {
        success = process_directory(input_folder, output_folder);
    } else {
        success = ensure_directory(output_folder) &&
                  process_file(input_path, output_folder);
    }
    free(default_output);
    return success ? 0 : 1;
}
