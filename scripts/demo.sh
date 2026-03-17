# TG11-OS demo script
# Works with: run demo.sh (or run -x demo.sh)

echo --- TG11 DEMO START ---
echo Version: $(version)
echo Working dir: $(pwd)

if ($(version) == "v0.0.5")
  echo You are on the latest known version.
elif ($(version) == "v0.0.4")
  echo You are on an older version. Please upgrade to v0.0.5.
else
  echo Unknown version detected: $(version)
fi

touch demo.txt
write demo.txt Hello from TG11 demo script on $(version)
echo demo.txt says:
cat demo.txt

foreach item in alpha,beta,gamma do echo loop item = $(item)

echo --- TG11 DEMO END ---
