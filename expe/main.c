#pragma warning(disable: 4101)

#include "exp/exp_eval.h"
#include "exp/exp_eval_utils.h"



#include <stdlib.h>
#ifdef _WIN32
# include <crtdbg.h>
#endif

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include <fileu.h>

#include <argparse.h>
#include <fswatcher/fswatcher.h>






static int mainReturn(int r)
{
#if !defined(NDEBUG) && defined(_WIN32)
    system("pause");
#endif
    return r;
}


int main(int argc, char* argv[])
{
#if !defined(NDEBUG) && defined(_WIN32)
    _CrtSetDbgFlag(_CRTDBG_ALLOC_MEM_DF | _CRTDBG_LEAK_CHECK_DF);
#endif

    char* entryFile = NULL;
    bool watchFlag = false;
    struct argparse_option options[] =
    {
        OPT_HELP(),
        //OPT_GROUP("Basic options"),
        OPT_STRING('f', "file", &entryFile, "execute entry file"),
        OPT_BOOLEAN('w', "watch", &watchFlag, "watch file and execute it when it changes"),
        OPT_END(),
    };
    struct argparse argparse;
    argparse_init(&argparse, options, NULL, 0);
    argc = argparse_parse(&argparse, argc, argv);

    if (entryFile)
    {
        if (watchFlag)
        {
            fswatcher_t watcher = fswatcher_create(FSWATCHER_CREATE_DEFAULT, FSWATCHER_EVENT_ALL, entryFile, NULL);

            fswatcher_destroy(watcher);
        }
        else
        {
            EXP_EvalContext* ctx = EXP_newEvalContext(NULL);
            bool r = EXP_evalFile(ctx, entryFile, true);
            EXP_EvalError err = EXP_evalLastError(ctx);
            if (r)
            {
                assert(EXP_EvalErrCode_NONE == err.code);
            }
            else
            {
                EXP_evalErrorFprint(stderr, &err);
            }
            EXP_evalDataStackFprint(stdout, ctx);
            EXP_evalContextFree(ctx);
        }
    }
    else
    {
        argparse_usage(&argparse);
    }

    return mainReturn(EXIT_SUCCESS);
}






























































