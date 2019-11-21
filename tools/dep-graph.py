#!/usr/bin/python3
# Args: dep-graph <synchk|verchk> root.dfy output.dot
# Gather the syntax or verification check output for all dfy files reachable
# from root.dfy. Construct a GraphViz dot file as output.

import os
from lib_deps import *
from lib_aggregate import *

class Traverser:
    def __init__(self, reportType, rootDfy, outputFilename):
        self.reportType = reportType
        self.output = []
        self.count = 0
        self.output.append("digraph {")

        self.visited = set()
        root = IncludeReference(None, 0, rootDfy)
        self.visit(root)

        self.addFillColors()

        self.createSubgraphs()

        self.output.append("}")
        self.emit(outputFilename)

    def visit(self, iref):
        self.count += 1
        #print("visiting %d of %d" % (self.count, len(self.visited)))
        #print("as normpath: %d" % len(set([i.normPath for i in self.visited])))
        if iref in self.visited:
            return
        self.visited.add(iref)
        for dep in childrenForIref(iref):
            self.output.append('"%s" -> "%s";' % (iref.normPath, dep.normPath))
        for dep in childrenForIref(iref):
            self.visit(dep)

    def getSummary(self, iref):
        report = os.path.join(ROOT_PATH, "build", iref.normPath).replace(".dfy", "."+self.reportType)
        return summarize(self.reportType, report)

    def addFillColors(self):
        def breakName(name):
            parts = name.rsplit("/", 1)
            return "/\n".join(parts)

        for iref in self.visited:
            summary = self.getSummary(iref)
            self.output.append('"%s" [style=filled; %s; label="%s\n%ss"];' % (
                iref.normPath, summary.style, breakName(iref.normPath), summary.userTimeSec))

    def sourceDir(self, iref):
        return iref.normPath.rsplit("/", 1)[0]

    def createSubgraphs(self):
        prefixes = set([self.sourceDir(iref) for iref in self.visited])
        for prefix in prefixes:
            members = ['"%s"' % iref.normPath for iref in self.visited if self.sourceDir(iref) == prefix]
            dot_safe_prefix = prefix.replace("/", "_").replace("-", "_")
            # NB the cluster_ prefix is semantically important to graphviz
            # https://graphs.grevian.org/example#example-6
            self.output.append("subgraph cluster_%s {" % dot_safe_prefix)
            self.output.append('    label="%s"' % prefix)
            self.output.append("    style=filled")
            self.output.append("    color=lightblue")
            for member in members:
                self.output.append("    %s;" % member);
            self.output.append("}");

    def emit(self, outputFilename):
        fp = open(outputFilename, "w")
        for line in self.output:
            fp.write(line+"\n")
        fp.close()

def main():
    reportType = sys.argv[1]
    assert reportType in ("verchk", "synchk")
    rootDfy = sys.argv[2]
    outputFilename = sys.argv[3]
    Traverser(reportType, rootDfy, outputFilename)

main()
