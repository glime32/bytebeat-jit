/*
 * bytebeat — de la musique avec une seule formule, compilée en code machine.
 *
 * Le « bytebeat » : on calcule une expression entière de t (le numéro de
 * l'échantillon) 8000 fois par seconde, on garde l'octet du bas, et on
 * l'envoie à la carte son. t*(42&t>>10) fait déjà une mélodie.
 *
 * L'expression passe par trois moteurs :
 *   arbre : on parcourt l'arbre syntaxique à chaque échantillon ;
 *   pile  : on la compile en bytecode pour une petite machine à pile ;
 *   jit   : on la compile directement en instructions x86-64, écrites
 *           octet par octet dans une page mémoire rendue exécutable.
 * Les trois doivent donner exactement les mêmes octets (--verifier, --fuzz).
 */
#define _DEFAULT_SOURCE
#include <ctype.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

static void meurt(const char *m) {
    fprintf(stderr, "bytebeat : %s\n", m);
    exit(1);
}

/* ============================================================ sémantique */
/* Entiers 32 bits avec débordement « qui boucle », comme le C sur une vraie
 * machine (et comme JavaScript pour les opérations bit à bit). Pour ne pas
 * dépendre de comportements indéfinis du C, on calcule en non signé. Division
 * ou modulo par 0 : 0. Décalages : modulo 32. >> garde le signe. */

static int32_t op_div(int32_t a, int32_t b) {
    if (b == 0) return 0;
    if (b == -1) return (int32_t)(0u - (uint32_t)a);
    return a / b;
}
static int32_t op_mod(int32_t a, int32_t b) {
    if (b == 0 || b == -1) return 0;
    return a % b;
}
static int32_t op_shl(int32_t a, int32_t b) { return (int32_t)((uint32_t)a << (b & 31)); }
static int32_t op_sar(int32_t a, int32_t b) {
    int s = b & 31; /* décalage arithmétique, écrit sans dépendre du compilateur */
    return a >= 0 ? (int32_t)((uint32_t)a >> s) : (int32_t)~(~(uint32_t)a >> s);
}

/* ================================================================ lexique */

enum {
    J_FIN, J_NOMBRE, J_T, J_PG, J_PD, J_INTER, J_DEUXPTS,
    /* opérateurs binaires */
    J_OUOU, J_ETET, J_OU, J_XOR, J_ET, J_EGAL, J_DIFF, J_INF, J_INFEG, J_SUP, J_SUPEG,
    J_SHL, J_SHR, J_PLUS, J_MOINS, J_FOIS, J_DIV, J_MOD,
    /* unaires */
    J_TILDE, J_NON
};

typedef struct { int type; int32_t val; int pos; } Jeton;
static Jeton jetons[4096];
static int nb_jetons;
static const char *source;

static void erreur_a(int pos, const char *msg) {
    fprintf(stderr, "bytebeat : %s\n  %s\n  %*s^\n", msg, source, pos, "");
    exit(1);
}

static void analyse_lexicale(const char *s) {
    static const struct { const char *txt; int type; } ops[] = {
        {"||", J_OUOU}, {"&&", J_ETET}, {"==", J_EGAL}, {"!=", J_DIFF}, {"<=", J_INFEG}, {">=", J_SUPEG},
        {"<<", J_SHL}, {">>", J_SHR}, {"|", J_OU}, {"^", J_XOR}, {"&", J_ET}, {"<", J_INF}, {">", J_SUP},
        {"+", J_PLUS}, {"-", J_MOINS}, {"*", J_FOIS}, {"/", J_DIV}, {"%", J_MOD}, {"~", J_TILDE},
        {"!", J_NON}, {"(", J_PG}, {")", J_PD}, {"?", J_INTER}, {":", J_DEUXPTS},
    };
    source = s;
    int i = 0;
    nb_jetons = 0;
    while (1) {
        while (isspace((unsigned char)s[i])) i++;
        if (nb_jetons >= 4095) erreur_a(i, "expression trop longue");
        Jeton *j = &jetons[nb_jetons++];
        j->pos = i;
        if (!s[i]) { j->type = J_FIN; return; }
        if (isdigit((unsigned char)s[i])) {
            uint64_t v = 0;
            int base = 10;
            if (s[i] == '0' && (s[i + 1] == 'x' || s[i + 1] == 'X')) { base = 16; i += 2; }
            if (base == 16 && !isxdigit((unsigned char)s[i])) erreur_a(i, "chiffre hexadécimal attendu");
            while (base == 16 ? isxdigit((unsigned char)s[i]) : isdigit((unsigned char)s[i])) {
                int c = s[i++];
                v = v * base + (isdigit(c) ? c - '0' : (tolower(c) - 'a' + 10));
                v &= 0xFFFFFFFFu;
            }
            j->type = J_NOMBRE;
            j->val = (int32_t)(uint32_t)v;
            continue;
        }
        if (s[i] == 't' && !isalnum((unsigned char)s[i + 1])) { j->type = J_T; i++; continue; }
        int trouve = 0;
        for (size_t k = 0; k < sizeof ops / sizeof ops[0]; k++) {
            size_t n = strlen(ops[k].txt);
            if (!strncmp(s + i, ops[k].txt, n)) { j->type = ops[k].type; i += (int)n; trouve = 1; break; }
        }
        if (!trouve) erreur_a(i, "caractère inattendu (seuls t, des nombres et les opérateurs du C sont permis)");
    }
}

/* ========================================================== arbre syntaxique */

enum { N_NOMBRE, N_T, N_UNAIRE, N_BINAIRE, N_TERNAIRE };
typedef struct Noeud {
    int genre, op;
    int32_t val;
    struct Noeud *a, *b, *c;
} Noeud;

static Noeud *noeud(int genre, int op, Noeud *a, Noeud *b, Noeud *c) {
    Noeud *n = calloc(1, sizeof *n);
    if (!n) meurt("plus de mémoire");
    n->genre = genre; n->op = op; n->a = a; n->b = b; n->c = c;
    return n;
}

/* priorité des opérateurs binaires, comme en C (plus grand = plus fort) */
static int priorite(int t) {
    switch (t) {
    case J_OUOU: return 1;
    case J_ETET: return 2;
    case J_OU: return 3;
    case J_XOR: return 4;
    case J_ET: return 5;
    case J_EGAL: case J_DIFF: return 6;
    case J_INF: case J_INFEG: case J_SUP: case J_SUPEG: return 7;
    case J_SHL: case J_SHR: return 8;
    case J_PLUS: case J_MOINS: return 9;
    case J_FOIS: case J_DIV: case J_MOD: return 10;
    default: return 0;
    }
}

static int k_jeton;
static Noeud *expression(void);

static Noeud *primaire(void) {
    Jeton *j = &jetons[k_jeton];
    if (j->type == J_NOMBRE) { k_jeton++; Noeud *n = noeud(N_NOMBRE, 0, 0, 0, 0); n->val = j->val; return n; }
    if (j->type == J_T) { k_jeton++; return noeud(N_T, 0, 0, 0, 0); }
    if (j->type == J_PG) {
        k_jeton++;
        Noeud *n = expression();
        if (jetons[k_jeton].type != J_PD) erreur_a(jetons[k_jeton].pos, "parenthèse fermante attendue");
        k_jeton++;
        return n;
    }
    if (j->type == J_MOINS || j->type == J_TILDE || j->type == J_NON || j->type == J_PLUS) {
        k_jeton++;
        Noeud *a = primaire();
        return j->type == J_PLUS ? a : noeud(N_UNAIRE, j->type, a, 0, 0);
    }
    erreur_a(j->pos, j->type == J_FIN ? "expression incomplète" : "un nombre, t ou ( était attendu ici");
    return NULL;
}

/* « precedence climbing » : on avale les opérateurs de priorité >= min */
static Noeud *binaire(int min) {
    Noeud *g = primaire();
    while (1) {
        int op = jetons[k_jeton].type, p = priorite(op);
        if (p == 0 || p < min) return g;
        k_jeton++;
        Noeud *d = binaire(p + 1); /* associativité à gauche */
        g = noeud(N_BINAIRE, op, g, d, 0);
    }
}

static Noeud *expression(void) {
    Noeud *c = binaire(1);
    if (jetons[k_jeton].type != J_INTER) return c;
    k_jeton++;
    Noeud *a = expression();
    if (jetons[k_jeton].type != J_DEUXPTS) erreur_a(jetons[k_jeton].pos, "« : » attendu après « ? »");
    k_jeton++;
    Noeud *b = expression(); /* associatif à droite : a ? b : c ? d : e */
    return noeud(N_TERNAIRE, 0, c, a, b);
}

static Noeud *compile_texte(const char *s) {
    analyse_lexicale(s);
    k_jeton = 0;
    Noeud *n = expression();
    if (jetons[k_jeton].type != J_FIN) erreur_a(jetons[k_jeton].pos, "en trop");
    return n;
}

/* ============================================= moteur 1 : parcours d'arbre */

static int32_t binop(int op, int32_t a, int32_t b) {
    switch (op) {
    case J_PLUS: return (int32_t)((uint32_t)a + (uint32_t)b);
    case J_MOINS: return (int32_t)((uint32_t)a - (uint32_t)b);
    case J_FOIS: return (int32_t)((uint32_t)a * (uint32_t)b);
    case J_DIV: return op_div(a, b);
    case J_MOD: return op_mod(a, b);
    case J_ET: return a & b;
    case J_OU: return a | b;
    case J_XOR: return a ^ b;
    case J_SHL: return op_shl(a, b);
    case J_SHR: return op_sar(a, b);
    case J_INF: return a < b;
    case J_INFEG: return a <= b;
    case J_SUP: return a > b;
    case J_SUPEG: return a >= b;
    case J_EGAL: return a == b;
    case J_DIFF: return a != b;
    }
    return 0;
}

static int32_t unop(int op, int32_t a) {
    switch (op) {
    case J_MOINS: return (int32_t)(0u - (uint32_t)a);
    case J_TILDE: return ~a;
    case J_NON: return !a;
    }
    return a;
}

static int32_t eval_arbre(const Noeud *n, int32_t t) {
    switch (n->genre) {
    case N_NOMBRE: return n->val;
    case N_T: return t;
    case N_UNAIRE: return unop(n->op, eval_arbre(n->a, t));
    case N_TERNAIRE: return eval_arbre(n->a, t) ? eval_arbre(n->b, t) : eval_arbre(n->c, t);
    default:
        if (n->op == J_ETET) return eval_arbre(n->a, t) ? eval_arbre(n->b, t) != 0 : 0;
        if (n->op == J_OUOU) return eval_arbre(n->a, t) ? 1 : eval_arbre(n->b, t) != 0;
        return binop(n->op, eval_arbre(n->a, t), eval_arbre(n->b, t));
    }
}

/* pliage des constantes : 3*4+t devient 12+t avant toute compilation */
static int constantes_pliees;
static Noeud *plie(Noeud *n) {
    if (n->a) n->a = plie(n->a);
    if (n->b) n->b = plie(n->b);
    if (n->c) n->c = plie(n->c);
    int ok = n->genre != N_NOMBRE && n->genre != N_T;
    if (n->a && n->a->genre != N_NOMBRE) ok = 0;
    if (n->b && n->b->genre != N_NOMBRE) ok = 0;
    if (n->c && n->c->genre != N_NOMBRE) ok = 0;
    if (n->genre == N_TERNAIRE && n->a->genre == N_NOMBRE) { /* condition connue : on garde une branche */
        constantes_pliees++;
        return n->a->val ? n->b : n->c;
    }
    if (!ok) return n;
    int32_t v = eval_arbre(n, 0);
    constantes_pliees++;
    Noeud *r = noeud(N_NOMBRE, 0, 0, 0, 0);
    r->val = v;
    return r;
}

/* ================================================ moteur 2 : machine à pile */

enum { B_CONST, B_T, B_BIN, B_UN, B_SAUTZ, B_SAUT, B_BOOL, B_FIN };
typedef struct { uint8_t code, op; int32_t arg; } Instr;
static Instr prog[8192];
static int nb_instr;

static int emet(int code, int op, int32_t arg) {
    if (nb_instr >= 8192) meurt("programme trop long");
    prog[nb_instr] = (Instr){(uint8_t)code, (uint8_t)op, arg};
    return nb_instr++;
}

static void compile_pile(const Noeud *n) {
    switch (n->genre) {
    case N_NOMBRE: emet(B_CONST, 0, n->val); return;
    case N_T: emet(B_T, 0, 0); return;
    case N_UNAIRE: compile_pile(n->a); emet(B_UN, n->op, 0); return;
    case N_TERNAIRE: {
        compile_pile(n->a);
        int sz = emet(B_SAUTZ, 0, 0);
        compile_pile(n->b);
        int s = emet(B_SAUT, 0, 0);
        prog[sz].arg = nb_instr;
        compile_pile(n->c);
        prog[s].arg = nb_instr;
        return;
    }
    default:
        if (n->op == J_ETET || n->op == J_OUOU) { /* court-circuit : b n'est calculé que si besoin */
            Noeud vrai = {N_NOMBRE, 0, 1, 0, 0, 0}, faux = {N_NOMBRE, 0, 0, 0, 0, 0};
            Noeud b_bool = {N_UNAIRE, J_NON, 0, 0, 0, 0}, b_bool2 = {N_UNAIRE, J_NON, 0, &b_bool, 0, 0};
            b_bool.a = n->b; /* !!b vaut 0 ou 1 */
            Noeud t = {N_TERNAIRE, 0, 0, n->a, n->op == J_ETET ? &b_bool2 : &vrai, n->op == J_ETET ? &faux : &b_bool2};
            compile_pile(&t);
            return;
        }
        compile_pile(n->a);
        compile_pile(n->b);
        emet(B_BIN, n->op, 0);
    }
}

static int32_t eval_pile(int32_t t) {
    int32_t pile[256];
    int sp = 0;
    for (int pc = 0;;) {
        const Instr *i = &prog[pc++];
        switch (i->code) {
        case B_CONST: pile[sp++] = i->arg; break;
        case B_T: pile[sp++] = t; break;
        case B_BIN: sp--; pile[sp - 1] = binop(i->op, pile[sp - 1], pile[sp]); break;
        case B_UN: pile[sp - 1] = unop(i->op, pile[sp - 1]); break;
        case B_SAUTZ: if (!pile[--sp]) pc = i->arg; break;
        case B_SAUT: pc = i->arg; break;
        case B_FIN: return pile[sp - 1];
        }
    }
}

static int profondeur(const Noeud *n) { /* place de pile nécessaire */
    if (!n) return 0;
    int a = profondeur(n->a), b = profondeur(n->b) + 1, c = profondeur(n->c);
    int m = a > b ? a : b;
    return m > c ? m : c;
}

/* ===================================================== moteur 3 : JIT x86-64 */
/* Convention : t arrive dans edi, le résultat repart dans eax. Pour a OP b :
 * on calcule b, on l'empile, on calcule a, on dépile b dans ecx, puis
 * « op eax, ecx ». Si b est une constante ou t, pas besoin de la pile. */

static uint8_t *code;
static size_t code_n, code_cap;

static void o(int n, ...) {
    __builtin_va_list ap;
    __builtin_va_start(ap, n);
    for (int i = 0; i < n; i++) {
        if (code_n >= code_cap) meurt("code machine trop long");
        code[code_n++] = (uint8_t)__builtin_va_arg(ap, int);
    }
    __builtin_va_end(ap);
}
static void o32(int32_t v) { o(4, v & 0xFF, (v >> 8) & 0xFF, (v >> 16) & 0xFF, (v >> 24) & 0xFF); }
static size_t saut32(int cond) { /* jz rel32 (cond=1) ou jmp rel32 (cond=0) ; renvoie où corriger */
    if (cond) o(2, 0x0F, 0x84); else o(1, 0xE9);
    o32(0);
    return code_n - 4;
}
static void corrige32(size_t ou) {
    int32_t rel = (int32_t)(code_n - (ou + 4));
    memcpy(code + ou, &rel, 4);
}
static size_t saut8(int opcode) { o(2, opcode, 0); return code_n - 1; }
static void corrige8(size_t ou) { code[ou] = (uint8_t)(code_n - (ou + 1)); }

static void jit_noeud(const Noeud *n);

/* met b dans ecx (et a dans eax) */
static int simple(const Noeud *n) { return n->genre == N_NOMBRE || n->genre == N_T; }

static void jit_operandes(const Noeud *a, const Noeud *b) {
    if (b->genre == N_NOMBRE) { jit_noeud(a); o(1, 0xB9); o32(b->val); }      /* mov ecx, imm32 */
    else if (b->genre == N_T) { jit_noeud(a); o(2, 0x89, 0xF9); }              /* mov ecx, edi */
    else if (simple(a)) { jit_noeud(b); o(2, 0x89, 0xC1); jit_noeud(a); }      /* mov ecx, eax ; puis a */
    else { jit_noeud(b); o(1, 0x50); jit_noeud(a); o(1, 0x59); }               /* push rax ... pop rcx */
}

static int commutatif(int op) {
    return op == J_PLUS || op == J_FOIS || op == J_ET || op == J_OU || op == J_XOR || op == J_EGAL || op == J_DIFF;
}

static void jit_division(int modulo) {
    /* b == 0 -> 0 ; b == -1 -> -a (ou 0 pour le modulo) : idiv planterait sur INT_MIN / -1 */
    o(2, 0x85, 0xC9);                       /* test ecx, ecx */
    size_t zero = saut8(0x74);              /* jz .zero */
    o(3, 0x83, 0xF9, 0xFF);                 /* cmp ecx, -1 */
    size_t moins1 = saut8(0x74);            /* je .moins1 */
    o(1, 0x99);                             /* cdq */
    o(2, 0xF7, 0xF9);                       /* idiv ecx */
    if (modulo) o(2, 0x89, 0xD0);           /* mov eax, edx */
    size_t fin1 = saut8(0xEB);              /* jmp .fin */
    corrige8(zero);
    o(2, 0x31, 0xC0);                       /* .zero: xor eax, eax */
    size_t fin2 = saut8(0xEB);
    corrige8(moins1);
    if (modulo) o(2, 0x31, 0xC0); else o(2, 0xF7, 0xD8); /* .moins1: 0 ou neg eax */
    corrige8(fin1);
    corrige8(fin2);
}

static void jit_booleen(int setcc) { o(3, 0x0F, setcc, 0xC0); o(3, 0x0F, 0xB6, 0xC0); } /* setcc al ; movzx eax, al */

static void jit_noeud(const Noeud *n) {
    switch (n->genre) {
    case N_NOMBRE: o(1, 0xB8); o32(n->val); return;          /* mov eax, imm32 */
    case N_T: o(2, 0x89, 0xF8); return;                      /* mov eax, edi */
    case N_UNAIRE:
        jit_noeud(n->a);
        if (n->op == J_MOINS) o(2, 0xF7, 0xD8);              /* neg eax */
        else if (n->op == J_TILDE) o(2, 0xF7, 0xD0);         /* not eax */
        else { o(2, 0x85, 0xC0); jit_booleen(0x94); }        /* test ; sete */
        return;
    case N_TERNAIRE: {
        jit_noeud(n->a);
        o(2, 0x85, 0xC0);                                    /* test eax, eax */
        size_t sinon = saut32(1);
        jit_noeud(n->b);
        size_t fin = saut32(0);
        corrige32(sinon);
        jit_noeud(n->c);
        corrige32(fin);
        return;
    }
    }
    if (n->op == J_ETET) {
        jit_noeud(n->a);
        o(2, 0x85, 0xC0);
        size_t fin = saut32(1);                               /* a nul : eax vaut déjà 0 */
        jit_noeud(n->b);
        o(2, 0x85, 0xC0);
        jit_booleen(0x95);                                    /* setne */
        corrige32(fin);
        return;
    }
    if (n->op == J_OUOU) {
        jit_noeud(n->a);
        o(2, 0x85, 0xC0);
        size_t calcule_b = saut32(1);
        o(1, 0xB8); o32(1);                                  /* mov eax, 1 */
        size_t fin = saut32(0);
        corrige32(calcule_b);
        jit_noeud(n->b);
        o(2, 0x85, 0xC0);
        jit_booleen(0x95);
        corrige32(fin);
        return;
    }
    if (commutatif(n->op) && simple(n->a) && !simple(n->b)) jit_operandes(n->b, n->a); /* 42&x -> x&42 */
    else jit_operandes(n->a, n->b);
    switch (n->op) {
    case J_PLUS: o(2, 0x01, 0xC8); break;                    /* add eax, ecx */
    case J_MOINS: o(2, 0x29, 0xC8); break;                   /* sub eax, ecx */
    case J_FOIS: o(3, 0x0F, 0xAF, 0xC1); break;              /* imul eax, ecx */
    case J_ET: o(2, 0x21, 0xC8); break;                      /* and eax, ecx */
    case J_OU: o(2, 0x09, 0xC8); break;                      /* or eax, ecx */
    case J_XOR: o(2, 0x31, 0xC8); break;                     /* xor eax, ecx */
    case J_SHL: o(2, 0xD3, 0xE0); break;                     /* shl eax, cl (le processeur garde cl & 31) */
    case J_SHR: o(2, 0xD3, 0xF8); break;                     /* sar eax, cl */
    case J_DIV: jit_division(0); break;
    case J_MOD: jit_division(1); break;
    default: {
        o(2, 0x39, 0xC8);                                    /* cmp eax, ecx */
        int cc = n->op == J_INF ? 0x9C : n->op == J_INFEG ? 0x9E : n->op == J_SUP ? 0x9F
               : n->op == J_SUPEG ? 0x9D : n->op == J_EGAL ? 0x94 : 0x95;
        jit_booleen(cc);
    }
    }
}

typedef int32_t (*Fonction)(int32_t);

static Fonction jit(const Noeud *n) {
#if defined(__x86_64__)
    if (code) munmap(code, code_cap);
    code_cap = 1 << 20;
    code = mmap(NULL, code_cap, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (code == MAP_FAILED) meurt("mmap impossible");
    code_n = 0;
    jit_noeud(n);
    o(1, 0xC3);                                               /* ret */
    /* W^X : la page n'est jamais à la fois inscriptible et exécutable */
    if (mprotect(code, code_cap, PROT_READ | PROT_EXEC)) meurt("mprotect impossible (système qui interdit le JIT ?)");
    Fonction f;
    void *p = code;
    memcpy(&f, &p, sizeof f);
    return f;
#else
    (void)n;
    return NULL;
#endif
}

/* =============================================================== sorties */

static void ecrit_wav(FILE *f, const uint8_t *e, uint32_t n, uint32_t taux) {
    uint8_t h[44] = {'R', 'I', 'F', 'F'};
    uint32_t v[] = {36 + n, 0, 0, 16, 1 | 1u << 16, taux, taux, 1 | 8u << 16, 0, n};
    memcpy(h + 4, &v[0], 4);
    memcpy(h + 8, "WAVEfmt ", 8);
    memcpy(h + 16, &v[3], 4);
    memcpy(h + 20, &v[4], 4);
    memcpy(h + 24, &v[5], 4);
    memcpy(h + 28, &v[6], 4);
    memcpy(h + 32, &v[7], 4);
    memcpy(h + 36, "data", 4);
    memcpy(h + 40, &v[9], 4);
    fwrite(h, 1, 44, f);
    fwrite(e, 1, n, f);
}

static double maintenant(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec * 1e-9;
}

/* ==================================================== expressions au hasard */

static uint64_t etat_hasard = 0x9E3779B97F4A7C15ull;
static uint32_t hasard(void) {
    etat_hasard ^= etat_hasard << 13; etat_hasard ^= etat_hasard >> 7; etat_hasard ^= etat_hasard << 17;
    return (uint32_t)(etat_hasard >> 16);
}

static int ecrit_hasard(char *s, int prof) {
    static const char *bin[] = {"+", "-", "*", "/", "%", "&", "|", "^", "<<", ">>", "<", "<=", ">", ">=", "==", "!=", "&&", "||"};
    static const char *un[] = {"-", "~", "!"};
    uint32_t r = hasard() % 10;
    if (prof <= 0 || r < 2) {
        if (hasard() % 2) return sprintf(s, "t");
        static const int32_t speciaux[] = {0, 1, -1, 31, 32, 255, 2147483647};
        if (hasard() % 4 == 0) return sprintf(s, "%" PRId32, speciaux[hasard() % 7]);
        return sprintf(s, "%u", hasard() % 300);
    }
    if (r < 3) { int k = sprintf(s, "%s(", un[hasard() % 3]); k += ecrit_hasard(s + k, prof - 1); return k + sprintf(s + k, ")"); }
    if (r < 4) {
        int k = sprintf(s, "(");
        k += ecrit_hasard(s + k, prof - 1); k += sprintf(s + k, "?");
        k += ecrit_hasard(s + k, prof - 1); k += sprintf(s + k, ":");
        k += ecrit_hasard(s + k, prof - 1);
        return k + sprintf(s + k, ")");
    }
    int k = sprintf(s, "(");
    k += ecrit_hasard(s + k, prof - 1);
    k += sprintf(s + k, "%s", bin[hasard() % 18]);
    k += ecrit_hasard(s + k, prof - 1);
    return k + sprintf(s + k, ")");
}

/* ===================================================================== main */

static void aide(void) {
    puts("bytebeat — de la musique avec une formule, compilée en code machine\n\n"
         "  bytebeat \"t*(42&t>>10)\" --jouer              écouter (aplay, pw-play ou paplay)\n"
         "  bytebeat \"t*(42&t>>10)\" -o son.wav [-d 30]   enregistrer 30 s\n"
         "  bytebeat \"t*(42&t>>10)\" --code               voir le bytecode et le code x86-64\n"
         "  bytebeat \"t*(42&t>>10)\" --bench              comparer arbre / pile / JIT\n"
         "  bytebeat \"t*(42&t>>10)\" --verifier           les 3 moteurs donnent-ils les mêmes octets ?\n"
         "  bytebeat --fuzz 2000                         idem sur 2000 expressions tirées au hasard\n\n"
         "  -r HZ         fréquence d'échantillonnage (défaut 8000)\n"
         "  --moteur M    arbre, pile ou jit (défaut jit)\n\n"
         "  t, nombres (décimal ou 0x..), + - * / % & | ^ << >> ~ ! < <= > >= == != && || ?:");
}

int main(int argc, char **argv) {
    const char *expr = NULL, *sortie = NULL, *moteur = "jit";
    int taux = 8000, jouer = 0, voir_code = 0, bench = 0, verifier = 0, fuzz = 0;
    double duree = 30;
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "-o") && i + 1 < argc) sortie = argv[++i];
        else if (!strcmp(a, "-d") && i + 1 < argc) duree = atof(argv[++i]);
        else if (!strcmp(a, "-r") && i + 1 < argc) taux = atoi(argv[++i]);
        else if (!strcmp(a, "--moteur") && i + 1 < argc) moteur = argv[++i];
        else if (!strcmp(a, "--jouer")) jouer = 1;
        else if (!strcmp(a, "--code")) voir_code = 1;
        else if (!strcmp(a, "--bench")) bench = 1;
        else if (!strcmp(a, "--verifier")) verifier = 1;
        else if (!strcmp(a, "--fuzz") && i + 1 < argc) fuzz = atoi(argv[++i]);
        else if (!strcmp(a, "-h") || !strcmp(a, "--aide")) { aide(); return 0; }
        else if (!expr) expr = a;
        else { fprintf(stderr, "argument en trop : %s\n", a); return 1; }
    }
    if (taux < 1000 || taux > 192000) meurt("fréquence entre 1000 et 192000 Hz");
#if !defined(__x86_64__)
    if (!strcmp(moteur, "jit")) moteur = "pile"; /* le JIT ne sait écrire que du x86-64 */
#endif

    if (fuzz) {
        /* expressions au hasard : les 3 moteurs doivent toujours être d'accord */
        static char buf[1 << 16];
        long essais = 0;
        for (int k = 0; k < fuzz; k++) {
            ecrit_hasard(buf, 1 + (int)(hasard() % 7));
            Noeud *n = compile_texte(buf);
            nb_instr = 0;
            compile_pile(n);
            emet(B_FIN, 0, 0);
            Fonction f = jit(n);
            Noeud *p = plie(compile_texte(buf));
            for (int s = 0; s < 400; s++) {
                int32_t t = s < 200 ? s * 977 - 5000 : (int32_t)hasard();
                int32_t x = eval_arbre(n, t), y = eval_pile(t), z = f ? f(t) : y, w = eval_arbre(p, t);
                essais++;
                if (x != y || x != z || x != w) {
                    fprintf(stderr, "DÉSACCORD sur %s pour t=%" PRId32 " : arbre %" PRId32 ", pile %" PRId32
                            ", jit %" PRId32 ", plié %" PRId32 "\n", buf, t, x, y, z, w);
                    return 1;
                }
            }
        }
        printf("%d expressions au hasard, %ld valeurs de t : arbre, pile, JIT et arbre plié sont d'accord\n", fuzz, essais);
        return 0;
    }

    if (!expr) { aide(); return 1; }
    Noeud *n = plie(compile_texte(expr));
    if (profondeur(n) > 200) meurt("expression trop imbriquée");
    nb_instr = 0;
    compile_pile(n);
    emet(B_FIN, 0, 0);
    Fonction f = jit(n);

    if (voir_code) {
        static const char *noms[] = {"const", "t", "bin", "un", "sautz", "saut", "bool", "fin"};
        printf("%d constante(s) pliée(s)\n\nbytecode (%d instructions) :\n", constantes_pliees, nb_instr);
        for (int i = 0; i < nb_instr; i++) {
            printf("  %3d  %-6s", i, noms[prog[i].code]);
            if (prog[i].code == B_CONST || prog[i].code == B_SAUTZ || prog[i].code == B_SAUT) printf(" %" PRId32, prog[i].arg);
            if (prog[i].code == B_BIN || prog[i].code == B_UN) {
                static const char *sym[] = {[J_OUOU] = "||", [J_ETET] = "&&", [J_OU] = "|", [J_XOR] = "^", [J_ET] = "&",
                    [J_EGAL] = "==", [J_DIFF] = "!=", [J_INF] = "<", [J_INFEG] = "<=", [J_SUP] = ">", [J_SUPEG] = ">=",
                    [J_SHL] = "<<", [J_SHR] = ">>", [J_PLUS] = "+", [J_MOINS] = "-", [J_FOIS] = "*", [J_DIV] = "/",
                    [J_MOD] = "%", [J_TILDE] = "~", [J_NON] = "!"};
                printf(" %s", prog[i].code == B_UN && prog[i].op == J_MOINS ? "neg" : sym[prog[i].op]);
            }
            putchar('\n');
        }
        if (f) {
            printf("\ncode x86-64 (%zu octets) :\n ", code_n);
            for (size_t i = 0; i < code_n; i++) printf(" %02x", code[i]);
            putchar('\n');
            /* désassemblage par objdump s'il est là */
            char chemin[] = "/tmp/bytebeat-XXXXXX";
            int fd = mkstemp(chemin);
            if (fd >= 0) {
                if (write(fd, code, code_n) == (ssize_t)code_n) {
                    char cmd[256];
                    snprintf(cmd, sizeof cmd, "objdump -D -b binary -mi386:x86-64 -M intel %s 2>/dev/null | sed -n '/<.data>:/,$p' | tail -n +2", chemin);
                    printf("\n");
                    fflush(stdout);
                    if (system(cmd) != 0) puts("(objdump absent : pas de désassemblage)");
                }
                close(fd);
                unlink(chemin);
            }
        }
        return 0;
    }

    if (verifier) {
        Noeud *brut = compile_texte(expr);
        for (int32_t t = 0; t < 1 << 22; t++) {
            int32_t a = eval_arbre(brut, t), b = eval_pile(t), c = f ? f(t) : b;
            if (a != b || a != c) {
                fprintf(stderr, "désaccord à t=%" PRId32 " : arbre %" PRId32 ", pile %" PRId32 ", jit %" PRId32 "\n", t, a, b, c);
                return 1;
            }
        }
        printf("arbre, pile et JIT : mêmes valeurs pour t = 0 .. %d\n", (1 << 22) - 1);
        return 0;
    }

    if (bench) {
        const int32_t N = 1 << 24;
        volatile uint32_t puits = 0;
        const char *noms[] = {"arbre", "pile", "jit"};
        double temps[3];
        for (int m = 0; m < 3; m++) {
            if (m == 2 && !f) { temps[2] = 0; break; }
            uint32_t acc = 0;
            double t0 = maintenant();
            for (int32_t t = 0; t < N; t++)
                acc += (uint8_t)(m == 0 ? eval_arbre(n, t) : m == 1 ? eval_pile(t) : f(t));
            temps[m] = maintenant() - t0;
            puits += acc;
            printf("  %-5s %6.1f M échantillons/s  (%.0f× le temps réel à %d Hz)\n", noms[m], N / temps[m] / 1e6, N / temps[m] / taux, taux);
        }
        if (f) printf("  le JIT va %.1f× plus vite que l'arbre et %.1f× plus vite que la pile\n", temps[0] / temps[2], temps[1] / temps[2]);
        (void)puits;
        return 0;
    }

    int32_t (*calcule)(int32_t) = !strcmp(moteur, "arbre") ? NULL : !strcmp(moteur, "pile") ? eval_pile : f;
    if (!strcmp(moteur, "arbre")) calcule = NULL;
#define ECHANTILLON(t) ((uint8_t)(calcule ? calcule(t) : eval_arbre(n, t)))

    if (sortie) {
        uint32_t total = (uint32_t)(duree * taux);
        uint8_t *e = malloc(total ? total : 1);
        if (!e) meurt("plus de mémoire");
        for (uint32_t t = 0; t < total; t++) e[t] = ECHANTILLON((int32_t)t);
        FILE *fo = fopen(sortie, "wb");
        if (!fo) meurt("impossible d'écrire le fichier");
        ecrit_wav(fo, e, total, (uint32_t)taux);
        fclose(fo);
        fprintf(stderr, "%.1f s à %d Hz -> %s\n", duree, taux, sortie);
        free(e);
        return 0;
    }

    if (jouer) {
        char cmd[256];
        /* la fréquence se place entre avant et apres */
        const struct { const char *avant, *apres; } lecteurs[] = {
            {"aplay -q -f U8 -c 1 -r ", " -"},
            {"pw-play --format u8 --channels 1 --rate ", " -"},
            {"paplay --raw --format=u8 --channels=1 --rate=", ""},
        };
        FILE *p = NULL;
        for (int k = 0; k < 3 && !p; k++) {
            char nom[32];
            sscanf(lecteurs[k].avant, "%31s", nom);
            snprintf(cmd, sizeof cmd, "command -v %s >/dev/null 2>&1", nom);
            if (system(cmd) != 0) continue;
            snprintf(cmd, sizeof cmd, "%s%d%s", lecteurs[k].avant, taux, lecteurs[k].apres);
            p = popen(cmd, "w");
        }
        if (!p) meurt("aucun lecteur audio trouvé (aplay, pw-play ou paplay)");
        fprintf(stderr, "lecture de %s (Ctrl-C pour arrêter)\n", expr);
        uint8_t bloc[1024];
        for (int32_t t = 0; duree <= 0 || t < duree * taux;) {
            for (int k = 0; k < 1024; k++, t++) bloc[k] = ECHANTILLON(t);
            if (fwrite(bloc, 1, sizeof bloc, p) != sizeof bloc) break;
        }
        pclose(p);
        return 0;
    }

    /* par défaut : octets bruts sur la sortie standard, pour  | aplay -f U8 */
    if (isatty(1)) { aide(); return 1; }
    uint8_t bloc[4096];
    for (int32_t t = 0;; ) {
        for (int k = 0; k < 4096; k++, t++) bloc[k] = ECHANTILLON(t);
        if (fwrite(bloc, 1, sizeof bloc, stdout) != sizeof bloc) return 0;
    }
}
