/*##############################################################################

    HPCC SYSTEMS software Copyright (C) 2012 HPCC Systems.

    Licensed under the Apache License, Version 2.0 (the "License");
    you may not use this file except in compliance with the License.
    You may obtain a copy of the License at

       http://www.apache.org/licenses/LICENSE-2.0

    Unless required by applicable law or agreed to in writing, software
    distributed under the License is distributed on an "AS IS" BASIS,
    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
    See the License for the specific language governing permissions and
    limitations under the License.
############################################################################## */

#include <stdio.h>
#include "jlog.hpp"
#include "jfile.hpp"
#include "jargv.hpp"
#include "jregexp.hpp"

#include "rmtfile.hpp"

#include "build-config.h"

#include "eclcmd.hpp"
#include "eclcmd_common.hpp"
#include "eclcmd_core.hpp"

#define ECLCC_ECLBUNDLE_PATH "ECLCC_ECLBUNDLE_PATH"
#define HPCC_FILEHOOKS_PATH "HPCC_FILEHOOKS_PATH"
#define VERSION_SUBDIR "_versions"

#define ECLOPT_DETAILS "--details"
#define ECLOPT_DRYRUN "--dryrun"
#define ECLOPT_FORCE "--force"
#define ECLOPT_KEEPPRIOR "--keepprior"
#define ECLOPT_RECURSE "--recurse"
#define ECLOPT_UPDATE "--update"


static StringBuffer bundlePath;
static StringBuffer hooksPath;
static bool optVerbose;


/*
 * Version comparison code.
 *
 * Version strings are of the form X.Y.Z
 *
 * Each component X, Y, Z etc should be numeric, with optional trailing alphanumeric.
 * Comparision is done numerically on the leading digits, and alphabetically on any remainder
 *
 * When comparing versions, 1.5.2 is considered to match 1.5 (but not vice versa)
 */

static int versionCompareSingleComponent(const char *v1, const char *v2)
{
    char *ev1;
    char *ev2;
    long l1 = strtoul(v1, &ev1, 10);
    long l2 = strtoul(v2, &ev2, 10);
    if (l1 != l2)
        return l1 > l2 ? -1 : 1;
    return stricmp(ev1, ev2);
}

static int versionCompare(const char *v1, const char *v2, bool strict)
{
    StringArray a1, a2;
    a1.appendList(v1, ".");
    a2.appendList(v2, ".");
    int i = 0;
    loop
    {
        if (a1.isItem(i) && a2.isItem(i))
        {
            int diff = versionCompareSingleComponent(a1.item(i), a2.item(i));
            if (diff)
                return diff;
            i++;
        }
        else if (a2.isItem(i))
            return 1;
        else if (strict && a2.isItem(i))  // Note: trailing info in v1 is ignored if not strict
            return -1;
        else
            return 0;
    }
}

static bool versionOk(const char *versionPresent, const char *minOk, const char *maxOk = NULL)
{
    if (minOk && versionCompare(versionPresent, minOk, false) > 0)
        return false;
    if (maxOk && versionCompare(versionPresent, maxOk, false) < 0)
        return false;
    return true;
}

//--------------------------------------------------------------------------------------------------------------

static void splitFilenameEx(const char *fullName, StringBuffer *drive, StringBuffer *path, StringBuffer *name, StringBuffer *ext, bool longExt)
{
    StringBuffer myext, myname;
    if (longExt)
    {
        if (!name)
            name = &myname;
        if (!ext)
            ext = &myext;
    }
    splitFilename(fullName, drive, path, name, ext);
    if (longExt)
    {
        // splitFileName treats the last dot as indicating the extension - we want to split at the first.
        const char *firstDot = strchr(*name, '.');
        if (firstDot)
        {
            ext->insert(0, firstDot);
            name->setLength(firstDot - name->str());
        }
    }
}

unsigned doEclCommand(StringBuffer &output, const char *cmd, const char *input, bool optVerbose)
{
    try
    {
        Owned<IPipeProcess> pipe = createPipeProcess();
        VStringBuffer runcmd("eclcc  --nologfile --nostdinc %s", cmd);
        pipe->run("eclcc", runcmd, ".", input != NULL, true, true, 1024 * 1024);
        if (optVerbose)
            printf("Running %s\nwith input %s\n", runcmd.str(), input);
        if (input)
        {
            pipe->write(strlen(input), input);
            pipe->closeInput();
        }
        StringBuffer error;
        while (true)
        {
            char buf[1024];
            size32_t read = pipe->readError(sizeof(buf), buf);
            if (!read)
                break;
            error.append(read, buf);
        }
        while (true)
        {
            char buf[1024];
            size32_t read = pipe->read(sizeof(buf), buf);
            if (!read)
                break;
            output.append(read, buf);
        }
        int ret = pipe->wait();
        if (optVerbose && (ret > 0 || error.length()))
            printf("eclcc return code was %d, output to stderr:\n%s", ret, error.str());
        return ret;
    }
    catch (IException *E)
    {
        E->Release();
        output.clear();
        return 255;
    }
}

static void extractValueFromEnvOutput(StringBuffer &path, const char *specs, const char *name)
{
    StringBuffer search(name);
    search.append('=');
    const char *found = strstr(specs, search);
    if (found)
    {
        found += search.length();
        const char *eol = strchr(found, '\n');
        if (eol)
        {
            while (eol >= found)
            {
                if (!isspace(*eol))
                    break;
                eol--;
            }
            path.append(eol - found + 1, found);
        }
        else
            path.append(found);
    }
    else
    {
        found = getenv(name);
        if (!found)
            throw MakeStringException(0, "%s could not be located", name);
        path.append(found);
    }
}

void recursiveRemoveDirectory(IFile *dir, bool isDryRun)
{
    Owned<IDirectoryIterator> files = dir->directoryFiles(NULL, false, true);
    ForEach(*files)
    {
        IFile *thisFile = &files->query();
        if (thisFile->isDirectory()==foundYes)
            recursiveRemoveDirectory(thisFile, isDryRun);
        if (isDryRun || optVerbose)
            printf("rm %s\n", thisFile->queryFilename());
        if (!isDryRun)
            thisFile->remove();
    }
    if (isDryRun || optVerbose)
        printf("rmdir %s\n", dir->queryFilename());
    if (!isDryRun)
        rmdir(dir->queryFilename());
}

static void getCompilerPaths()
{
    StringBuffer output;
    doEclCommand(output, "-showpaths", NULL, false);
    extractValueFromEnvOutput(bundlePath, output, ECLCC_ECLBUNDLE_PATH);
    extractValueFromEnvOutput(hooksPath, output, HPCC_FILEHOOKS_PATH);
}

//--------------------------------------------------------------------------------------------------------------

interface IBundleCollection;

interface IBundleInfo : extends IInterface
{
    virtual bool isValid() const = 0;
    virtual const char *queryName() const = 0;
    virtual const char *queryVersion() const = 0;
    virtual const char *queryCleanVersion() const = 0;
    virtual const char *queryDescription() const = 0;
    virtual unsigned numDependencies() const = 0;
    virtual const char *queryDependency(unsigned idx) const = 0;
    virtual void printError() const = 0;
    virtual void printFullInfo() const = 0;
    virtual void printShortInfo() const = 0;
    virtual void checkValid() const = 0;
    virtual bool checkVersion(const char *required) const = 0;
    virtual bool checkDependencies(const IBundleCollection &allBundles, const IBundleInfo *bundle, bool going) const = 0;

    virtual const char *queryInstalledPath() const = 0;
    virtual void setInstalledPath(const char *path) = 0;
    virtual void setActive(bool active) = 0;
};

interface IBundleInfoSet : extends IInterface
{
    virtual const char *queryName() const = 0;
    virtual const IBundleInfo *queryActive() = 0;
    virtual unsigned numVersions() = 0;
    virtual const IBundleInfo &queryVersion(const char *version) = 0;
    virtual const IBundleInfo &queryVersion(unsigned idx) = 0;
    virtual void deleteVersion(const char *version, bool isDryRun) = 0;
    virtual void deleteAllVersions(bool isDryRun) = 0;
};

interface IBundleCollection : extends IInterface
{
    virtual unsigned numBundles() const = 0;
    virtual IBundleInfoSet *queryBundleSet(const char *name) const = 0;
    virtual IBundleInfoSet *queryBundleSet(unsigned idx) const = 0;
    virtual const IBundleInfo *queryBundle(const char *name) const = 0;  // returns active
    virtual bool checkDependencies(const IBundleInfo *bundle, bool going) const = 0;
};

class CBundleInfo : public CInterfaceOf<IBundleInfo>
{
public:
    CBundleInfo(const char *bundle, bool fromFile)
    {
        active = false;
        try
        {
            StringBuffer output;
            StringBuffer eclOpts("-Me -");
            StringBuffer bundleName;
            Owned<IFile> bundleFile = createIFile(bundle);
            if (fromFile)
            {
                if (bundleFile->exists())
                {
                    eclOpts.append(" --nobundles");
                    StringBuffer drive, path;
                    splitFilenameEx(bundle, &drive, &path, &bundleName, NULL, true);
                    if (bundleFile->isDirectory())
                        eclOpts.appendf(" -I%s", bundle);
                    else if (drive.length() + path.length())
                        eclOpts.appendf(" -I%s%s", drive.str(), path.str());
                    else
                        eclOpts.appendf(" -I.");
                }
                else
                    throw MakeStringException(0, "File not found");
            }
            else
            {
                bundleName.append(bundle);
                active = true;
            }
            VStringBuffer bundleCmd("IMPORT %s.Bundle as B;"
                                    " [ (UTF8) B.name, (UTF8) B.version, B.description, B.license, B.copyright ] +"
                                    " [ (UTF8) COUNT(b.authors) ] + B.authors + "
                                    " [ (UTF8) COUNT(B.dependsOn) ] + B.dependsOn;", bundleName.str());
            if (doEclCommand(output, eclOpts.str(), bundleCmd, optVerbose)>2)
                throw MakeStringException(0, "%s cannot be resolved as a bundle\n", bundle);
            // output should contain [ 'name', 'version', etc ... ]
            if (optVerbose)
                printf("Bundle info from ECL compiler: %s\n", output.str());
            if (!output.length())
                throw MakeStringException(0, "%s cannot be resolved as a bundle\n", bundle);
            RegExpr re("'{[^'\r\n\\\\]|\\\\[^\r\n]}*'");
            extractAttr(re, name, output.str());
            extractAttr(re, version);
            extractAttr(re, description);
            extractAttr(re, license);
            extractAttr(re, copyright);
            extractArray(re, authors);
            extractArray(re, depends);
            cleanVersion.append(version).replace('.', '_');  // MORE - other illegal chars too...
            if (isdigit(cleanVersion.charAt(0)))
                cleanVersion.insert(0, "V");
        }
        catch (IException *E)
        {
            exception.setown(E);
        }
    }
    virtual void checkValid() const { if (exception) throw exception.getLink(); }
    virtual bool isValid() const { return exception == NULL; }
    virtual const char *queryName() const { return name.get(); }
    virtual const char *queryVersion() const { return version.get(); }
    virtual const char *queryCleanVersion() const { return cleanVersion.str(); }
    virtual const char *queryDescription() const { return description.get(); }
    virtual unsigned numDependencies() const { return depends.ordinality(); }
    virtual const char *queryDependency(unsigned idx) const { return depends.item(idx); }
    virtual const char *queryInstalledPath() const { return installedPath.get(); }
    virtual void setInstalledPath(const char *path) { installedPath.set(path); }
    virtual void setActive(bool isActive) { active = isActive; }
    virtual void printError() const
    {
        if (exception)
        {
            StringBuffer s;
            exception->errorMessage(s);
            printf("%s", s.str());
        }
    }
    virtual void printFullInfo() const
    {
        printAttr("Name:", name);
        printAttr("Version:", version);
        printAttr("Description:", description);
        printAttr("License:", license);
        printAttr("Copyright:", copyright);
        printArray("Authors:", authors);
        printArray("DependsOn:", depends);
    }
    virtual void printShortInfo() const
    {
        if (!active)
            printf("(");
        printf("%-13s %-10s %s", name.get(), version.get(), description.get());
        if (!active)
            printf(")");
        printf("\n");
    }
    virtual bool checkVersion(const char *required) const
    {
        StringArray requiredVersions;
        requiredVersions.appendList(required, "-");
        const char *minOk, *maxOk;
        minOk = requiredVersions.item(0);
        if (requiredVersions.isItem(1))
            maxOk = requiredVersions.item(0);
        else
            maxOk = NULL;
        return versionOk(version, minOk, maxOk);
    }
    virtual bool checkDependencies(const IBundleCollection &allBundles, const IBundleInfo *bundle, bool going) const
    {
        bool ok = true;
        for (int i = 0; i < depends.length(); i++)
        {
            if (!checkDependency(allBundles, depends.item(i), bundle, going))
                ok = false;
        }
        return ok;
    }
private:
    bool checkDependency(const IBundleCollection &allBundles, const char *dep, const IBundleInfo *bundle, bool going) const
    {
        StringArray depVersions;
        depVersions.appendList(dep, " ");
        const char *dependentName = depVersions.item(0);
        if (bundle && !strieq(dependentName, bundle->queryName()))
            return true;
        Linked <const IBundleInfo> dependentBundle(bundle);
        if (!dependentBundle)
        {
            dependentBundle.setown(allBundles.queryBundle(dependentName));
            if (!dependentBundle || !dependentBundle->isValid())
            {
                printf("%s requires %s, which cannot be loaded\n", name.get(), dependentName);
                return false;
            }
        }
        if (going)
        {
            printf("%s is required by %s\n", dependentName,  name.get());
            return false;
        }
        else if (depVersions.length() > 1)
        {
            bool ok = false;
            for (int i = 1; i < depVersions.length(); i++)
                if (dependentBundle->checkVersion(depVersions.item(i)))
                    ok = true;
            if (!ok)
                printf("%s requires %s, version %s found\n", name.get(), dep, dependentBundle->queryVersion());
            return ok;
        }
        return true;
    }

    static void printAttr(const char *prompt, const StringAttr &attr)
    {
        printf("%-13s%s\n", prompt, attr.get());
    }
    static void printArray(const char *prompt, const StringArray &array)
    {
        if (array.length())
        {
            printf("%-13s", prompt);
            ForEachItemIn(idx, array)
            {
                if (idx)
                    printf(", ");
                printf("%s", array.item(idx));
            }
            printf("\n");
        }
    }
    void extractAttr(RegExpr &re, StringAttr &dest, const char *searchIn = NULL)
    {
        const char *found = re.find(searchIn);
        if (!found)
            throw MakeStringException(0, "Unexpected response from eclcc\n");
        StringBuffer cleaned;
        found++;  // skip the '
        unsigned foundLen = re.findlen()-2; // and the trailing ''
        for (int i = 0; i < foundLen; i++)
        {
            unsigned char c = found[i];
            if (c=='\\')
            {
                i++;
                c = found[i];
                switch (c)
                {
                case 't': c = '\t'; break;
                case 'r': c = '\r'; break;
                case 'n': c = '\n'; break;
                default:
                    if (isdigit(c))
                    {
                        unsigned value = c - '0';
                        value = value * 8 + (found[++i] - '0');
                        value = value * 8 + (found[++i] - '0');
                        c = value;
                    }
                    break;
                }
            }
            cleaned.append(c);
        }
        dest.set(cleaned);
    }
    void extractArray(RegExpr &re, StringArray &dest)
    {
        StringAttr temp;
        extractAttr(re, temp);
        unsigned count = atoi(temp);
        while (count--)
        {
            extractAttr(re, temp);
            dest.append(temp);
        }
    }

    StringAttr installedPath;
    StringAttr name;
    StringAttr version;
    StringBuffer cleanVersion;
    StringAttr description;
    StringAttr license;
    StringAttr copyright;
    StringArray authors;
    StringArray depends;
    Owned<IException> exception;
    static MapStringToMyClass<IBundleInfo> cache;
    bool active;
};

MapStringToMyClass<IBundleInfo> CBundleInfo::cache(true);

class CBundleInfoSet : public CInterfaceOf<IBundleInfoSet>
{
public:
    CBundleInfoSet(const char *_path, const char *_bundlePath)
        : path(_path)
    {
        loaded = false;
        active = false;
        splitFilename(path, NULL, NULL, &name, NULL);
        redirectFileName.append(_bundlePath);
        addPathSepChar(redirectFileName).append(name).append(".ecl");
    }
    virtual const char *queryName() const
    {
        return name.str();
    }
    virtual const IBundleInfo *queryActive()
    {
        checkLoaded();
        // MORE - could support bundles with no active version - later
        return &bundles.item(0);
    }
    virtual unsigned numVersions()
    {
        checkLoaded();
        return bundles.length();
    }
    virtual const IBundleInfo &queryVersion(const char *version)
    {
        checkLoaded();
        return bundles.item(findVersion(version));
    }
    virtual const IBundleInfo &queryVersion(unsigned idx)
    {
        checkLoaded();
        return bundles.item(idx);
    }
    virtual void deleteVersion(const char *version, bool isDryRun)
    {
        checkLoaded();
        deleteBundle(findVersion(version), isDryRun);
    }
    virtual void deleteAllVersions(bool isDryRun)
    {
        checkLoaded();
        ForEachItemInRev(idx, bundles)
        {
            deleteBundle(idx, isDryRun);
        }
        if (isDryRun && bundles.length() > 1)
            printf("rmdir %s\n", path.get());
    }
private:
    unsigned findVersion(const char *version)
    {
        assertex(version && *version);
        ForEachItemIn(idx, bundles)
        {
            if (strieq(version, bundles.item(idx).queryVersion()))
                return idx;
        }
        throw MakeStringException(0, "No version %s found in %s", path.get(), version);
    }
    void deleteBundle(unsigned idx, bool isDryRun)
    {
        const IBundleInfo &goer = bundles.item(idx);
        const char *installedDir = goer.queryInstalledPath();
        Owned<IFile> versionDir = createIFile(installedDir);
        recursiveRemoveDirectory(versionDir, isDryRun);
        if (!isDryRun)
            bundles.remove(idx);
        if (bundles.length() == isDryRun ? 1 : 0)
        {
            Owned<IFile> versionsDir = createIFile(path);
            recursiveRemoveDirectory(versionsDir, isDryRun);
        }
    }
    void checkLoaded()
    {
        if (!loaded)
        {
            StringBuffer activeBundlePath(path);
            Owned<IBundleInfo> activeBundle =  new CBundleInfo(redirectFileName, true);
            if (activeBundle->isValid())
            {
                addPathSepChar(activeBundlePath).append(activeBundle->queryCleanVersion());
                activeBundle->setInstalledPath(activeBundlePath);
                activeBundle->setActive(true);
                bundles.append(*activeBundle.getClear());
                active = true;
            }
            Owned<IFile> versionsDir = createIFile(path);
            Owned<IDirectoryIterator> versions = versionsDir->directoryFiles(NULL, false, true);
            ForEach (*versions)
            {
                IFile &f = versions->query();
                const char *vname = f.queryFilename();
                if (f.isDirectory() && vname && vname[0] != '.')
                {
                    if (!streq(activeBundlePath, vname))
                    {
                        Owned<IDirectoryIterator> bundleFiles = f.directoryFiles(NULL, false, true);  // Should contain a single file - either a .ecl file or a directory
                        ForEach (*bundleFiles)
                        {
                            Owned<IBundleInfo> bundle = new CBundleInfo(bundleFiles->query().queryFilename(), true);
                            bundle->setInstalledPath(vname);
                            bundles.append(*bundle.getClear());
                            break;
                        }
                    }
                }
            }
            loaded = true;
        }
    }
    StringAttr path;
    StringBuffer redirectFileName;
    StringBuffer name;
    IArrayOf<IBundleInfo> bundles;
    bool loaded;
    bool active;
};

class CBundleCollection : public CInterfaceOf<IBundleCollection>
{
public:
    CBundleCollection(const char *_bundlePath) : bundlePath(_bundlePath)
    {
        if (optVerbose)
            printf("Using bundle path %s\n", bundlePath.get());
        Owned<IFile> bundleDir = createIFile(bundlePath);
        if (bundleDir->exists())
        {
            if (!bundleDir->isDirectory())
                throw MakeStringException(0, "Bundle path %s does not specify a directory", bundlePath.get());
            StringBuffer versionsPath(bundlePath);
            addPathSepChar(versionsPath).append(VERSION_SUBDIR);
            Owned<IFile> versionsDir = createIFile(versionsPath);
            Owned<IDirectoryIterator> versions = versionsDir->directoryFiles(NULL, false, true);
            ForEach (*versions)
            {
                IFile &f = versions->query();
                const char *name = f.queryFilename();
                if (f.isDirectory() && name && name[0] != '.')
                    bundleSets.append(*new CBundleInfoSet(name, bundlePath));
            }
        }
        bundleSets.sort(compareBundleSets);
    }
    virtual unsigned numBundles() const
    {
        return bundleSets.length();
    }
    virtual IBundleInfoSet *queryBundleSet(const char *name) const
    {
        // MORE - at some point a hash table might be called for?
        assertex(name && *name);
        ForEachItemIn(idx, bundleSets)
        {
            if (strieq(name, bundleSets.item(idx).queryName()))
                return &bundleSets.item(idx);
        }
        return NULL;
    }
    virtual IBundleInfoSet *queryBundleSet(unsigned idx) const
    {
        return &bundleSets.item(idx);
    }
    virtual const IBundleInfo *queryBundle(const char *name) const
    {
        IBundleInfoSet *bundleSet = queryBundleSet(name);
        if (!bundleSet)
            return NULL;
        const IBundleInfo *bundle = bundleSet->queryActive();
        if (!bundle || !bundle->isValid())
            return NULL;
        return bundle;
    }
    virtual bool checkDependencies(const IBundleInfo *bundle, bool going) const
    {
        bool ok = true;
        ForEachItemIn(idx, bundleSets)
        {
            const IBundleInfo *active = bundleSets.item(idx).queryActive();
            if (active && active->isValid() && !active->checkDependencies(*this, bundle, going))
                ok = false;
        }
        return ok;
    }

private:
    static int compareBundleSets(IInterface **a, IInterface **b)
    {
        IBundleInfoSet *aa = (IBundleInfoSet *) *a;
        IBundleInfoSet *bb = (IBundleInfoSet *) *b;
        return stricmp(aa->queryName(), bb->queryName());
    }
    StringAttr bundlePath;
    IArrayOf<IBundleInfoSet> bundleSets;
};


//-------------------------------------------------------------------------------------------------

class EclCmdBundleList : public EclCmdCommon
{
public:
    EclCmdBundleList()
    {
        optDetails = false;
    }

    virtual bool parseCommandLineOptions(ArgvIterator &iter)
    {
        for (; !iter.done(); iter.next())
        {
            const char *arg = iter.query();
            if (matchCommandLineOption(iter, true) != EclCmdOptionMatch)
                return false;
        }
        return true;
    }

    virtual eclCmdOptionMatchIndicator matchCommandLineOption(ArgvIterator &iter, bool finalAttempt)
    {
        if (iter.matchFlag(optDetails, ECLOPT_DETAILS))
            return EclCmdOptionMatch;
        return EclCmdCommon::matchCommandLineOption(iter, finalAttempt);
    }

    virtual bool finalizeOptions(IProperties *globals)
    {
        if (!EclCmdCommon::finalizeOptions(globals))
            return false;
        ::optVerbose = optVerbose;
        return true;
    }

    virtual int processCMD()
    {
        CBundleCollection allBundles(bundlePath);
        for (unsigned i = 0; i < allBundles.numBundles(); i++)
        {
            IBundleInfoSet *bundleSet = allBundles.queryBundleSet(i);
            if (optDetails)
            {
                for (unsigned j = 0; j < bundleSet->numVersions(); j++)
                {
                    const IBundleInfo &bundle = bundleSet->queryVersion(j);
                    if (bundle.isValid())
                       bundle.printShortInfo();
                    else
                       printf("Bundle %s[%d] could not be loaded\n", bundleSet->queryName(), j);
                }
            }
            else
            {
                StringBuffer name(bundleSet->queryName());
                name.toLowerCase();
                name.setCharAt(0, toupper(name.charAt(0)));
                printf("%s\n", name.str());
            }
        }
        return 0;
    }
    virtual void usage()
    {
        fputs("\nUsage:\n"
              "\n"
              "The 'list' command will list all installed bundles\n"
              "\n"
              "ecl bundle list \n"
              " Options:\n"
              "   --details          Report details of each installed bundle\n",
        stdout);
        EclCmdCommon::usage(false);
    }
protected:
    bool optDetails;
};

//-------------------------------------------------------------------------------------------------

class EclCmdBundleBase : public EclCmdCommon
{
public:
    virtual bool parseCommandLineOptions(ArgvIterator &iter)
    {
        for (; !iter.done(); iter.next())
        {
            const char *arg = iter.query();
            if (*arg != '-')
            {
                if (optBundle.isEmpty())
                    optBundle.set(arg);
                else
                {
                    fprintf(stderr, "\nunrecognized argument %s\n", arg);
                    return false;
                }
                continue;
            }
            if (matchCommandLineOption(iter, true) != EclCmdOptionMatch)
                return false;
        }
        return true;
    }
    virtual bool finalizeOptions(IProperties *globals)
    {
        if (optBundle.isEmpty())
            return false;
        if (!EclCmdCommon::finalizeOptions(globals))
            return false;
        ::optVerbose = optVerbose;
        return true;
    }
protected:
    bool isFromFile() const
    {
        // If a supplied bundle id contains pathsep or ., assume a filename is being supplied
        return strchr(optBundle, PATHSEPCHAR) != NULL ||strchr(optBundle, '.') != NULL;
    }
    StringAttr optBundle;
};

//-------------------------------------------------------------------------------------------------

class EclCmdBundleDepends : public EclCmdBundleBase
{
public:
    virtual int processCMD()
    {
        CBundleCollection allBundles(bundlePath);
        ConstPointerArray active;
        Owned<const IBundleInfo> bundle;
        if (isFromFile())
            bundle.setown(new CBundleInfo(optBundle, true));
        else
            bundle.set(allBundles.queryBundle(optBundle));
        if (!bundle || !bundle->isValid())
            throw MakeStringException(0, "Bundle %s could not be loaded", optBundle.get());
        return printDependency(allBundles, bundle, 0, active) ? 0 : 1;
    }

    virtual eclCmdOptionMatchIndicator matchCommandLineOption(ArgvIterator &iter, bool finalAttempt)
    {
        if (iter.matchFlag(optRecurse, ECLOPT_RECURSE))
            return EclCmdOptionMatch;
        return EclCmdCommon::matchCommandLineOption(iter, finalAttempt);
    }

    virtual void usage()
    {
        fputs("\nUsage:\n"
              "\n"
              "The 'depends' command will show the dependencies of a bundle\n"
              "\n"
              "ecl bundle depends <bundle> \n"
              " Options:\n"
              "   <bundle>            The name of an installed bundle, or of a bundle file\n"
              "   --recurse           Display indirect dependencies\n",
        stdout);
        EclCmdCommon::usage();
    }
private:
    bool printDependency(const IBundleCollection &allBundles, const IBundleInfo *bundle, int level, ConstPointerArray active)
    {
        if (active.find(bundle))
            throw MakeStringException(0, "Circular dependency detected");
        bool ok = true;
        active.append(bundle);
        unsigned numDependencies = bundle->numDependencies();
        for (unsigned i = 0; i < numDependencies; i++)
        {
            for (int l = 0; l < level; l++)
                printf(" ");
            const char *dependency = bundle->queryDependency(i);
            StringArray depVersions;
            depVersions.appendList(dependency, " ");
            const char *dependentName = depVersions.item(0);
            const IBundleInfo *dep = allBundles.queryBundle(dependentName);
            printf("%s%s\n", dependentName, dep ? "" : " (not found)");
            if (optRecurse)
            {
                ok = printDependency(allBundles, dep, level + 1, active) && ok;
            }
        }
        active.pop();
        return ok;
    }

    bool optRecurse;
};

//-------------------------------------------------------------------------------------------------

class EclCmdBundleInfo : public EclCmdBundleBase
{
public:
    virtual int processCMD()
    {
        Owned<const IBundleInfo> bundle;
        if (isFromFile())
            bundle.setown(new CBundleInfo(optBundle, true));
        else
        {
            CBundleCollection allBundles(bundlePath);
            IBundleInfoSet *bundleSet = allBundles.queryBundleSet(optBundle);
            if (!bundleSet || !bundleSet->queryActive())
                throw MakeStringException(0, "Bundle %s not found", optBundle.get());
            bundle.set(bundleSet->queryActive());
        }
        bundle->checkValid();
        bundle->printFullInfo();
        return 0;
    }

    virtual void usage()
    {
        fputs("\nUsage:\n"
              "\n"
              "The 'info' command will print information about a bundle\n"
              "\n"
              "ecl bundle info <bundle> \n"
              " Options:\n"
              "   <bundle>            The name of a bundle file\n",
        stdout);
        EclCmdCommon::usage();
    }
};

//-------------------------------------------------------------------------------------------------

class EclCmdBundleInstall : public EclCmdBundleBase
{
public:
    EclCmdBundleInstall()
    {
        optForce = false;
        optUpdate = false;
        optDryRun = false;
        optKeepPrior = false;
    }

    virtual eclCmdOptionMatchIndicator matchCommandLineOption(ArgvIterator &iter, bool finalAttempt)
    {
        if (iter.matchFlag(optDryRun, ECLOPT_DRYRUN))
            return EclCmdOptionMatch;
        if (iter.matchFlag(optForce, ECLOPT_FORCE))
            return EclCmdOptionMatch;
        if (iter.matchFlag(optKeepPrior, ECLOPT_KEEPPRIOR))
            return EclCmdOptionMatch;
        if (iter.matchFlag(optUpdate, ECLOPT_UPDATE))
            return EclCmdOptionMatch;
        return EclCmdBundleBase::matchCommandLineOption(iter, finalAttempt);
    }

    virtual int processCMD()
    {
        Owned<IFile> bundleFile = createIFile(optBundle.get());
        if (bundleFile->exists())
        {
            bool ok = true;
            bool removePrior = false;
            Owned<IBundleInfo> bundle = new CBundleInfo(optBundle, true);
            bundle->checkValid();
            printf("Installing bundle %s version %s\n", bundle->queryName(), bundle->queryVersion());

            CBundleCollection allBundles(bundlePath);
            IBundleInfoSet *oldBundleSet = allBundles.queryBundleSet(bundle->queryName());
            if (oldBundleSet)
            {
                // MORE - this is not right if there are multiple versions installed...
                const IBundleInfo *active = oldBundleSet->queryActive();
                if (active && active->isValid() && !optUpdate)
                {
                    printf("A bundle %s version %s is already installed\n", active->queryName(), active->queryVersion());
                    printf("Specify --update to install a replacement version of this bundle\n");
                    return 1;
                }
                int diff = versionCompare(bundle->queryVersion(), active->queryVersion(), true);
                if (diff >= 0)
                {
                    printf("Existing version %s is newer or same version\n", active->queryVersion());
                    ok = false;
                }
                else
                {
                    printf("Updating previously installed version %s", active->queryVersion());
                }
                if (diff == 0 || !optKeepPrior)
                {
                    if (optKeepPrior)
                        printf("--keepprior not possible when reinstalling same version\n");
                    removePrior = true;
                }
            }
            if (!bundle->checkDependencies(allBundles, NULL, false) || allBundles.checkDependencies(bundle, false))
                ok = false;
            if (!ok)
            {
                if (!optForce)
                {
                    printf("Specify --force to force installation of this bundle\n");
                    return 1;
                }
                else
                    printf("--force specified - updating anyway\n");
            }
            if (removePrior)
            {
                oldBundleSet->deleteAllVersions(optDryRun);
            }

            const char *version = bundle->queryCleanVersion();
            const char *name = bundle->queryName();

            if (!optDryRun && !recursiveCreateDirectory(bundlePath))
                throw MakeStringException(0, "Cannot create bundle directory %s", bundlePath.str());

            // Create the redirector file
            // MORE - only do this if we want to make the new one active...
            StringBuffer redirectPath(bundlePath);
            addPathSepChar(redirectPath).append(name).append(".ecl");
            if (optDryRun || optVerbose)
                printf("Create redirector file %s\n", redirectPath.str());
            if (!optDryRun)
            {
                VStringBuffer redirect("IMPORT %s.%s.%s.%s as _%s; EXPORT %s := _%s;", VERSION_SUBDIR, name, version, name, name, name, name);
                Owned<IFile> redirector = createIFile(redirectPath);
                Owned<IFileIO> rfile = redirector->open(IFOcreaterw);
                rfile->write(0, redirect.length(), redirect.str());
                bundle->setActive(true);
            }

            // Copy the bundle contents
            StringBuffer versionPath(bundlePath);
            addPathSepChar(versionPath).append(VERSION_SUBDIR).append(PATHSEPCHAR).append(name).append(PATHSEPCHAR).append(version);
            if (!recursiveCreateDirectory(versionPath))
                throw MakeStringException(0, "Cannot create bundle version directory %s", versionPath.str());
            if (bundleFile->isDirectory() == foundYes) // could also be an archive, acting as a directory
            {
                copyDirectory(bundleFile, versionPath);
            }
            else
            {
                StringBuffer tail;
                splitFilename(bundleFile->queryFilename(), NULL, NULL, &tail, &tail);
                versionPath.append(PATHSEPCHAR).append(tail);
                Owned<IFile> targetFile = createIFile(versionPath.str());
                if (targetFile->exists())
                    throw MakeStringException(0, "A bundle file %s is already installed", versionPath.str());
                if (optDryRun || optVerbose)
                    printf("cp %s %s\n", bundleFile->queryFilename(), targetFile->queryFilename());
                if (!optDryRun)
                    doCopyFile(targetFile, bundleFile, 1024 * 1024, NULL, NULL, false);
            }
            if (!optDryRun)
            {
                bundle->printShortInfo();
                printf("Installation complete\n");
            }
        }
        else
            throw MakeStringException(0, "%s cannot be resolved as a bundle\n", optBundle.get());
        return 0;
    }

    virtual void usage()
    {
        printf("\nUsage:\n"
                "\n"
                "The 'install' command will install a bundle\n"
                "\n"
                "ecl bundle install <bundle> \n"
                " Options:\n"
                "   <bundle>          The name of a bundle file\n"
                "   --dryrun          Print what would be installed, but do not copy\n"
                "   --force           Install even if required dependencies missing\n"
                "   --keepprior       Do not remove an previous versions of the bundle\n"
                "   --update          Update an existing installed bundle\n"
               );
        EclCmdCommon::usage();
    }
private:
    void copyDirectory(IFile *sourceDir, const char *destdir)
    {
        Owned<IDirectoryIterator> files = sourceDir->directoryFiles(NULL, false, true);
        ForEach(*files)
        {
            IFile *thisFile = &files->query();
            StringBuffer tail;
            splitFilename(thisFile->queryFilename(), NULL, NULL, &tail, &tail);
            StringBuffer destname(destdir);
            destname.append(PATHSEPCHAR).append(tail);
            if (thisFile->isDirectory()==foundYes)
            {
                copyDirectory(thisFile, destname);
            }
            else
            {
                Owned<IFile> targetFile = createIFile(destname);
                if (optDryRun || optVerbose)
                    printf("cp %s %s\n", thisFile->queryFilename(), targetFile->queryFilename());
                if (!optDryRun)
                    doCopyFile(targetFile, thisFile, 1024*1024, NULL, NULL, false);
            }
        }
    }
    bool optUpdate;
    bool optForce;
    bool optDryRun;
    bool optKeepPrior;
};

//-------------------------------------------------------------------------------------------------

class EclCmdBundleUninstall : public EclCmdBundleBase
{
public:
    EclCmdBundleUninstall()
    {
        optDryRun = false;
        optForce = false;
    }
    virtual int processCMD()
    {
        bool ok = true;
        CBundleCollection allBundles(bundlePath);
        IBundleInfoSet *goer = allBundles.queryBundleSet(optBundle);
        if (!goer)
        {
            printf("Bundle %s not found", optBundle.get());
            return 1;
        }
        const IBundleInfo *active = goer->queryActive();
        if (active && active->isValid())
            ok = allBundles.checkDependencies(active, true);
        if (optForce)
        {
            printf("--force specified - uninstalling anyway\n");
            ok = true;
        }
        if (ok)
        {
            // NOTE - this removes ALL versions of the bundle.
            // We probably want to change that when version checking gets more sophisticated!
            goer->deleteAllVersions(optDryRun);
            StringBuffer redirectPath(bundlePath);
            addPathSepChar(redirectPath).append(goer->queryName()).append(".ecl");
            Owned<IFile> redirector = createIFile(redirectPath);
            if (optDryRun || optVerbose)
                printf("rm %s\n", redirector->queryFilename());
            if (!optDryRun)
                redirector->remove();
        }
        return ok;
    }

    virtual eclCmdOptionMatchIndicator matchCommandLineOption(ArgvIterator &iter, bool finalAttempt)
    {
        if (iter.matchFlag(optDryRun, ECLOPT_DRYRUN))
            return EclCmdOptionMatch;
        if (iter.matchFlag(optForce, ECLOPT_FORCE))
            return EclCmdOptionMatch;
        return EclCmdCommon::matchCommandLineOption(iter, finalAttempt);
    }

    virtual void usage()
    {
        fputs("\nUsage:\n"
              "\n"
              "The 'depends' command will show the dependencies of a bundle\n"
              "\n"
              "ecl bundle uninstall <bundle> \n"
              " Options:\n"
              "   <bundle>            The name of an installed bundle\n"
                "   --dryrun          Print files that would be removed, but do not remove them\n"
                "   --force           Uninstall even if other bundles are dependent on this\n",
        stdout);
        EclCmdCommon::usage();
    }
private:
    bool optDryRun;
    bool optForce;
};

//-------------------------------------------------------------------------------------------------

IEclCommand *createBundleSubCommand(const char *cmdname)
{
    if (!cmdname || !*cmdname)
        return NULL;
    else if (strieq(cmdname, "depends"))
        return new EclCmdBundleDepends();
    else if (strieq(cmdname, "info"))
        return new EclCmdBundleInfo();
    else if (strieq(cmdname, "install"))
        return new EclCmdBundleInstall();
    else if (strieq(cmdname, "list"))
        return new EclCmdBundleList();
    else if (strieq(cmdname, "uninstall"))
        return new EclCmdBundleUninstall();
    return NULL;
}

//=========================================================================================

class BundleCMDShell : public EclCMDShell
{
public:
    BundleCMDShell(int argc, const char *argv[], EclCommandFactory _factory, const char *_version)
        : EclCMDShell(argc, argv, _factory, _version)
    {
    }

    virtual void usage()
    {
        fprintf(stdout, "\nUsage:\n\n"
                "ecl bundle <command> [command options]\n\n"
                "   bundle commands:\n"
                "      depends      show bundle dependencies\n"
                "      info         show bundle information\n"
                "      install      install a bundle\n"
                "      list         list installed bundles\n");
    }
};

static int doMain(int argc, const char *argv[])
{
    BundleCMDShell processor(argc, argv, createBundleSubCommand, BUILD_TAG);
    return processor.run();
}

int main(int argc, const char *argv[])
{
    assert(versionOk("1.4.2", "1.4"));
    assert(!versionOk("1.4.2", "1.5"));
    assert(versionOk("1.4.2", "1.4", "1.5"));
    assert(versionOk("1.5.2", "1.4", "1.5"));
    assert(!versionOk("1.6.2", "1.4", "1.5"));
    InitModuleObjects();
    removeLog();
    unsigned exitCode;
    try
    {
        getCompilerPaths();
        installFileHooks(hooksPath.str());
        exitCode = doMain(argc, argv);
    }
    catch (IException *E)
    {
        StringBuffer m;
        E->errorMessage(m);
        printf("Error: %s", m.str());
        E->Release();
        exitCode = 2;
    }
    removeFileHooks();
    releaseAtoms();
    exit(exitCode);
}
