# XMLSQL scripting tool.

## Overview

`xmlsql` is a simple command line tool which is used to produce XML format output from XML format input using special markup to allow inclusion of data and the results of SQL queries.

Basically, it is very useful for dynamic content web pages or svg or other XML based output.

It is not suitable as a general purpose language for processing input from forms - other tools such as `envcgi` and `envsql` make it easy to use shells such as `bash` or `csh` to process input. `xmlsql` can then easily be used as part of the script to produce output.

### Key features are:-

- You can include content of environment variables, or fields from SQL queries, in the output. There are a range of formatting options such as date and time formatting and money formatting.
- Fields used for input in forms are pre-loaded with correct values. This works not only for simple `<INPUT...>` tags, but for `CHECKBOX`, `RADIO`, and even `<SELECT...>/<OPTION...>` marking the right field(s) as selected.
- Variables can be set within the script, and tests done on variables (and SQL fields) at any time allowing selection of different output as required.
- SQL queries are iterated allowing lists to be created easily. SQL queries can even be nested.
- Simple loop control logic allow iterations using internal variables.
- Simple maths can be performed to allow totals for money, and counts in iterations.

## Command line

The standard `--help` argument lists the command line options. A list of input files can be specified allowing several files to be processed one after the other. Note that you cannot start something such as an sql query or an `<IF...>` in one file and end in the next, however you can set variables in one file and use them in the next if you wish.

Normally output is to stdout, but can be to a file. Input is from stdin by default. A common usage is input from stdin and the shell input in-line using `<<`

Database access is optional, and if no `<SQL...>` tags are used then not database controls need be specified.

There are `--debug` and `--comment` options which provide more information about what is happening and any errors.

## Variables

One of the key features is the use of variables. In some cases a variable can be referenced simply by name, such as in `<INPUT NAME=name...>`, but they can also be used within any attribute of any tag using the $ prefix. E.g. `<A HREF="test.cgi?X=$X">` where `$X` is expanded to the content of the variable `X`.

When looking up a variable, first the current SQL query (if any) is checked for an exact match to a field name. Any outer nested SQL queries are checked in order until a match is found. Then any locally set variables are checked, and finally any environment variables. Finding no match means an empty string for `$name` expansion and means no effect on the source for changes to `INPUT`/`SELECT`/`TEXTAREA` fields.

It is strongly recommended that the arguments for `xmlsql` are not processed by the shell, e.g. for csh use `<< 'END'` rather than `<< END`. Any variables expanded by the shell cannot be properly escaped by `xmlsql`.

In most cases the `$` expansion uses the rules as specified for the `xmlexpand` command. However, there are some exceptions.
(see https://github.com/revk/SQLlib/blob/master/sqlexpand.md for details of the `sqlexpand` processing)

- If an attribute value starts `$?name` then the whole attribute is ignored if the variable name does not exist. The `$?name` itself is not expanded. e.g. `tag="${?testvar}1234" will be either not present or `tag="1234"` depending on `testvar` existing or not. Used to make an attribute conditional.
- Nested expansion is allowed, e.g. `${$name}` gets `$name` and then gets a variable by that name.
- In the case of the tags for generating an SQL query, the operations are exactly the same as the `xmlexpand` command.

## Special tags

`xmlsql` alters the content of some standard HTML tags such as `INPUT`, `OPTION`, and `TEXTAREA`. It also includes some specific `xmlsql` tags SQL, IF, and WHILE. These tags can be prefixed with the namespace `XMLSQL:`, e.g. `<XMLSQL:IF...>` rather than just `<IF...>`.

The special tags `SQL`, `IF`, `WHILE` and `WHEN` all require the tag to be specifically closed. These tags, as well as `SELECT` and `TEXTAREA`, must not over lap (i.e. `<IF...>...<SQL...>...</IF>...</SQL>` is not valid). The end of each tag is matched to the first. If there is a mismatch with no end tag then the starting tag is not processed. And end tag with no start tag just shows in the output as is. If you use `--comment` or `--debug` an error is reported where this is found.

Note that these special tags can, and often do, overlap with other HTML elements with no problem.

## SQL

The `<SQL...>` tag is used to start an SQL query. You must have a corresponding `</SQL>` to mark the end of the content. There are attributes to the `<SQL...>` tag which define the query to be performed. The query is processed and each row of output causes everything between the `<SQL...>` tag and `</SQL>` to be processed for that line. Variables accessed refer to the field names returned by the query. Field names containing a dot (i.e. `table.field`) are treated as the name after the last dot. Duplicate field names find the first instance only so use AS in the SQL query to create different names where required.

**Important**: You need to use back quotes around field names or table names where they may be a reserved word in SQL. As later versions of SQL can make new reserved words it is recommended that back quoting is always used. Be careful using backquotes from shell stdin using `<<` though as the shell gives these meaning.

### SQL attributes

|Attribute|Meaning|
|---------|-------|
|`TABLE`|	This defined the table to be used and is placed after the `FROM` keyword in the query.|
|`FROM`|	As above|
|`SELECT`|	This defines the selected fields to be used and is placed after the `SELECT` keyword in the query. If missing, `*` is assumed.|
|`WHERE`|	This defines the `WHERE` clause in the query.|
|`ORDER`|	This defines the `ORDER` BY clause in the query.|
|`GROUP`|	This defines the `GROUP` BY clause in the query.|
|`LIMIT`|	This defines the `LIMIT` clause in the query.|
|`HAVING`|	This defines the `HAVING` clause in the query.|
|`QUERY`|	This defines the whole query, usually used for `CREATE TEMPORARY TABLE` type clauses.|
|`DESC`|	*(deprecated)* This appends `DESC` to the `ORDER` clause, causing the last field in the order list to be descending order.|
|`DISTINCT`|	*(deprecated)* Query prefixed with `DISTINCT`.|
|`KEY`|		This specifies a key field name - the `WHERE` clause is constructed as `fieldname=value` using the current value of the specified field if not in the environment.|
|`CSVHEAD`|	Include CSV row header line.|
|`CSV`|		If specified then the `<sql.../>` must be self closing. This creates an CSV style set of rows.|
|`XML`|		If specified then the `<sql.../>` must be self closing. This creates an XML style set of rows, each tagged using the value given to XML. E.g. `XML="row"`. Be careful in using select names and using `AS` to ensure valid XML tag names as this is not checked. If `XML` has no value (i.e. just `XML` not `XML=`) then generates Excel style table rows instead.|
|`JSARRAY`|	If specified then the `<sql...>` must be self closing. This creates a JSON array which contains arrays of the values from the query. If JSARRAY has a value, it is the name of a variable, and the entire JSON formatted output of the query is put in that variable.|
[`JSARRAYHEAD`|	If set, the first row in the `JSARRAY` output is an array of field names in order.|
|`JSON`|	If specified then the `<sql.../>` must be self closing. This creates a JSON array which contains objects with the tagged values from the query. If JSON has a value, it is the name of a variable, and the entire JSON formatted output of the query is put in that variable.|
|`TABLEHEAD`|	This creates a simple HTML table row with `<th>` tags for the column headings in the query.|
|`TABLEROW`|	If specified then the `<sql.../>` must be self closing. This creates simple HTML table rows with `<td>` tags for the column data in the result.|

### Special cases:-

- The use of `QUERY="..."` to perform an operation that has no result (e.g. a `CREATE TEMPORARY TABLE...` command) should be used in the form `<SQL QUERY="..." />` (i.e., with no content). If content is included it would never be used, so this is reported as an error.
- If the query has a result, even zero rows of result, then the `<SQL... />` format must not be used as the results of the query would have no output. Again, this is reported as an error.
- If the `ID="..."` attribute is used, and the field name specified in `ID` is not defined, then one row is shown with SQL default values (typically as a blank input form).
- All date and datetime values retrieved from the database that are zero are retrieved as a blank string and not the normal `0000-00-00`, etc.

## SET

`SET` allows a variable or variables to be defined. Each attribute is processed in turn.

- `name=value` sets a variable of the specified name to the specified value.
- `name` (with no `=`) unsets a variable of the specified name.

## EVAL

`EVAL` allows a variable or variables to be defined using simple maths. You can use `+`, `-`, `*` and `/` operators and parenthesis and work to any precision.

Note the special cases of `.=`, `#=`, `/=` and `!=` apply to the settings after them in the `EVAL`, i.e. you must put these before the settings to which they are to be applied. (default format `*`)

- `name=value` sets a variable of the specified name to the specified value evaluated using simple maths.
- `name` (with no `=`) unsets a variable of the specified name.
- `#=places` forces rounding to specified number of places for final result. This is often used for money, use `#=2` to ensure result is to 2 decimal places even if `.00` and bankers rounding is used. If you do not set a number of places with `#` then the maximum number of decimal places in any argument is used as the number of places for the final result. (format `=`)
- `/=places` limits final divide to number of places, but will use fewer places if the answer would have training zeros unless # is also set. (format `+`)
- `!=string` sets the default string to use if the evaluation is somehow invalid (e.g. not a valid sum, or divide by zero, etc).
- `.=format` sets the stringdecimal format type.

Examples `<EVAL X="1+2*3">` would set `X` to `7`. `<EVAL #=2 X="123*1.2">` would set `X` to `147.60`.

Note: the value is expanded with variable names e.g. $NAME as you would expect, but also expands without the leading $ e.g. `<EVAL X=A+B>`. Also, any expanded variables that are blank or missing are treated as zero.

Note: the sums are done with rational maths and rounded as a final divide.

Note: Bankers rounding is normally applied, prefix places with a letter: `T`=Truncate (to 0); `U`=Up (away from 0); `F`=Floor (to -ve); `C`=Ceiling (to +ve); `R`=Round to nearest but 0.5 away from 0; `B`=Bankers rounding to nearest but 0.5 to even. E.g. to round up to whole number, `#=U0`.

**Warning**: You should only provide well formed numeric expressions with correct balanced brackets. In general errors are reported by retuning a string starting with an `!`, but the exact behaviour in case of incorrect input is not defined and should not be relied on.

## INPUT

The `<INPUT...>` tag has the `NAME="..."` field checked for a valid variable. If a variable is found then the `INPUT` is changed so that the specified variable is the initial value. If the variable is not found, no change is made to the source. You can include value attribute to set a default value, or set attribute to specify the value even if a variable/field of the name specified exists.

- `RADIO` and `CHECKBOX` cause `CHECKED` to be defined or removed depending on `VALUE="..."`. If there is no `VALUE` defined, then `CHECKED` is set if the variable is not a blank string, otherwise it is removed. If the variable content contains TAB characters, then each of the strings between the tabs is considered to be a value, and `CHECKED` set for each `INPUT` where the `VALUE="..."` matches one of those strings.
- `INPUT TYPE=SUBMIT` is not changed
- Other types of `INPUT` have the content of `VALUE="..."` changed to the correct value. Also, if the variable is defined and matches an SQL field, and SIZE is not defined, then SIZE is set to the field size plus 1. Similarly for `MAXLENGTH` which is set to the field size. Size is not set for `TYPE=HIDDEN` as there is no point.
- Including `TRIM` in the input will remove trailing zeros from a decimal fraction in the input box.

## SELECT/OPTION

The `NAME="..."` from the `<SELECT...>` is checked as a variable name. If defined then the `<OPTION...>` tags within the `SELECT` are considered and changed. For each, the `VALUE` is either specified in the `<OPTION VALUE="...">` or as the text after `<OPTION>` - this is checked against the variable value and `SELECTED` added or removed from the `<OPTION...>` tag as appropriate. If the variable content contains TAB characters, then each of the strings between the tabs is considered to be a value, and `SELECT` set for each `OPTION` where the value matches one of those strings. You can override the use of the variable/field with set attribute.

## TEXTAREA

The `NAME="..."` from the `<TEXTAREA...>` is checked as a variable name. If defined then everything up to the corresponding `</TEXTAREA>` is replaced with the variable's content. If not defined then `FILE="..."` is checked for a file that exists, and if it does then everything within the textarea tag is replaced with the contents of the file.

Note that the `--safe` command line option stops the `FILE=...` argument from functioning as it may allow access to system files like `/etc/passwd`.

## OUTPUT

The `<OUTPUT...>` tag is used to produce output. It has several attributes that define what is to be output.

### Attributes

|Attribute|Meaning|
|---------|-------|
|`NAME`		|Field name to be output|
|`VALUE`	|Alternative `NAME="..."` allows a complete value which may have several variables to be output.|
|`FILE`		|Alternative `VALUE="..."` allows a complete value which is read from a file.|
|`TYPE`		|Type of output format to use.|
|`HREF`		|Defines that an `<A HREF="...">` and `</A>` are to surround the value output (if not an empty string) using the specified `HREF` value.|
|`TARGET`	|Where `HREF` is used, the `<A...>` tag includes `TARGET="..."` using this value.|
|`CLASS`	|If `HREF` defined, then this is the `CLASS` on the `HREF`, else this means a `<SPAN...>` with that `CLASS` surrounds the value (if not an empty string).|
|`STYLE`	|If `HREF` defined, then this is the `STYLE` on the `HREF`, else this means a `<SPAN...>` with that `STYLE` surrounds the value (if not an empty string).|
|`SIZE`		|Truncate output to specified number of characters as formatted, and adds `...` to end if truncated.|
|`RIGHT`	|Prepend spaces to simple text output to make `SIZE` wide|
|`MISSING`	|Value to assume if field is not defined.|
|`BLANK`	|Value to assume if field is an empty string.|
|`XML`		|Indicate that this output is for quoted xml use not html, so escaping is handled differently.|
|*other*	|All other attributes define an alternative string to use where the value matches the attribute name specified and is not one of the tags listed above. This is the old way to use this, see `MATCH` and `REPLACE`.|
|`MATCH`	|All tags after this are assumed to be replacement for the whole value of the output, e.g. `tag=value` means if the output is tag then it is changed the value. After `MATCH` none of the tags listed above are recognised, so you can replace an output of `CLASS` with something if `CLASS=value` is listed after `MATCH`.|
|`REPLACE`	|As `MATCH`, except any instance of the tag in the output is replaced with value. This is done after HTML or other escaping. e.g. `":)"="<img src='smiley.png'>"` would do smiley replacement in test.|
|`ISDDISABLED`	|Checks if `ISDISABLED` is set and if so added a `disabled=disabled` to and `INPUT` tag.|

Note that you can include, as the final attribute, a `$variable` to expand as additional attributes - use with care.

### TYPE values

|Type|Meaning|
|----|-------|
|`TIMESTAMP`|	Use time format `%d %b %Y %H:%M:%S`|
|`DATE`|	Use time format `%d %b %Y` , or `%FT%T%z` if xml output|
|`*%*`|		Anything with `%` in it is assumed to be a time format for *strftime*|
|`INTERVAL`|	Show an integer number of seconds Yesterday|
|`RECENT`|	Use a relative time format for a date or datetime|
|`MEGA`|	Show an integer value using `P`, `T`, `G`, `M` or `k` suffix depending on size. Note that if you include the attribute `KELVIN` then `K` is used instead of `k` for *kilo*.|
|`MEBI`|	Show an integer value using `Pi`, `Ti`, `Gi`, `Mi` or `Ki` suffix depending on size. Note that if you include the attribute `FAKESI`, then the prefixes are printed as if SI units instead of binary prefixes. `KELVIN` can also be used, as above. But `Ki` is the normal suffix in this case, oddly.|
|`COMMA`|	Show an number with commas are three digit spacing, e.g. `1,234,567`|
|`CASH`|	Show a number as cash, font red for negative, `&pound;` and forced two decimal places (bankers rounding applied).|
|`CASH`*currency*|	Show as `CASH`, but using currency `USD`, `EUR` or `GBP`.|
|`MASK`|	Show a CIDR bit count as a dotted quad IP4 format mask.|
|`NTH`|		Show a number followed by `st`, `nd`, `rd` or `th` as appropriate.|
|`IP`|		Show as IP address, reformatted as standard, so for example a decimal IP4 address is shown as a dotted quad format IPv4.|
|`UKTEL`|	Format a `+xxx` format number as a UK telephone number|
|`PENCE`|	Show a decimal number removing trailing zeros and optionally using `¼`, `½`, or `¾` where appropriate|
|`TRIM`|	Show a decimal number removing trailing zeros in the decimal fraction|
|`FLOOR`|	Truncate at decimal point / full stop.|
|`AGE`|		Show difference from now, single units appropriate to time to nearest `¼`. e.g. `5 minutes`, or `44½ years`, etc.|
|`YEARS`|	Show difference from now in years, e.g. age in years based on DOB.|
|`IDN`|		Converts to UTF-8 if in IDN format|
|`SURNAME`|	Surname from name, i.e. last word|
|`FORENAME`|	Forename from name, i.e. skip recognised title and return first word|
|`FORENAMES`|	Forenames from name, i.e. skip recognised title and remove last word|
|`TITLE`|	Title from name if we recognise one|

Note that a type can be prefixed with `+` or `-`. If prefixed `-` then output is suppressed if the value does not start with a `-`, and if it does start with a `-` then the `-` is skipped. If prefixed `+` then output is suppressed if output does start with a `-`.

## FORMAT

In addition to data types, there is a control for the formatting of the output, using `FORMAT=`.

|Formats|Meaning|
|-------|-------|
|`PS`|		Escape suitable for postscript use (`\` in front of `(` or `)` or `\`).|
|`JSON`|	Partial escaping suitable for JSON.|
|`RAW`|		Show with no escaping at all - this is not recommended for anything that could be sourced externally as it allows insertion attacks. In most cases `SAFE` is appropriate.|
|`SAFE`|	Allow any properly balanced markup except `<script>` and attributes starting on which are generally javascript. `SAFEMARKUP` combines with `MARKUP` expanding `www.` prefix, smilies, etc. as well.|
|`MARKUP`|	Output allowing specific HTML markup (see below)|
|`TEXTAREA`|	Output for use in a text area (e.g. newlines as newlines no breaks)|

Note, for backwards compatibility these can be used as `TYPE=` where there is no change to content just formatting requirements.

## INCLUDE

Takes one attribute `src=` specifying a filename to include at this point. File is only loaded once even if in a loop and not loaded at all if conditional and not processed. Balancing of statements is internal to each file.

Can alternatively take `var=` and variable name to simply include the content of that variable at this point. Note this is only done once, so uses the variable as first seen even in a loop.

## EXEC

If `--exec` specified then `EXEC` can be used. It has `cmd=` as first argument and `arg=` as subsequent arguments that are the command to run and the args to pass to it. Output from the command is placed directly in the output with no processing. e.g. `<EXEC cmd="/bin/echo" arg="hello" arg="$var" />`. If no `cmd=` or `arg=` then assumed to be an argument as is.

Before the `cmd=` you can include as a first attribute `INCLUDE`. If present the output of the exec is included at this point (and not re-run if in a loop), so processed as part of the script, otherwise it is simply output at this point.

## LATER

This is deprecated and my be withdrawn. It was needed for `envhtml` when nested SQL was not possible, so is *deprecated* now.

The `<LATER>` tag is removed, then all content to the corresponding `</LATER>` output with no changes and no variable expansion in attributes. The `</LATER>` is removed. This allows a section of the input to be enclosed within `<LATER>...</LATER>` tags so that the output can be run through `xmlsql` again a second time. The `<LATER>` tags can, of course, be nested.

## IF

The `<IF...>` tag is used to allow control of what is output and what is not. Each attribute in the `<IF...>` tag is considered, and if the result is true then the content between `<IF...>` and corresponding `</IF>` are processed as normal. If not true then the content between `<IF...>` and corresponding `</IF>` are not processed or displayed.

To work out if an `IF` tag is true or false, each attribute is considered in turn. Other than `NOT`, `AND` and `OR`, the attribute is considered to be a test. If the test is true, then the next attribute is considered. If the test is false then attributes are skipped up to and past the next `OR` attribute. If no `OR` is found then the `IF` is considered false. Finding an `OR` after a true test means the whole `IF` is considered true and no more attributes need be checked.

|Attribute|Meaning|
|---------|-------|
|`name`|	True if the variable exists.|
|`name=0`|	True of the variable specified by the name has a zero value, including `0000-00-00`, `0000-00-00 00:00:00`, `000000000000000`, `0.00`, etc. A *blank* string is also matched.|
|`name=value`|	True of the variable specified by the name has the value. Note that this is a textual comparison and no evaluation is done on value other than normal variable expansion.|
|`name=+value`|	True if the value of the name variable is alphabetically the same or after the value. Note this is a purely textual comparison.|
|`name=-value`|	True if the value of the name variable is alphabetically the same or before the value. Note this is a purely textual comparison.|
|`name=*value`|	True if the value of the name variable is the same or a substring of the value, or if the value is not an empty string, if the value is a substring of the value of the name variable.|
|`name==value`|	True if the numeric value of the name variable is the same as the numeric value of the value.|
|`name=#+value`|	True if the numeric value of the name variable is the same or greater than the numeric value of the value..|
|`name=#-value`|	True if the numeric value of the name variable is the same or less than the numeric value of the value.|
|`name=&value`|	True if the numeric value of the name variable has bits in common with the numeric value of the value (binary `AND`).|
|`NOT`|		Means the truth of the next attribute is inverted.|
|`AND`|		Does nothing - attributes next to each other are implicitly `AND`'d|
|`OR`|		A false attribute skips to (and past) the next `OR`. But if an `OR` is reached with the previous attribute having been true then the `IF` as a whole is considered to be true.|
|`EXISTS="filename"`|	Is true if the file exists and can be read|
|`ELSE`|	Is true if the previous IF processed was considered false and its content skipped. Usually used simply as `<IF ELSE>`|

Note: `NOT` cannot be used in front of `NOT`, `AND`, `OR` or at the end. It is valid in front of `ELSE`, e.g. `<IF...>A</IF>B<IF NOT ELSE>C</IF>` would either produce `B` or `ABC` depending on the initial `IF` condition. Normally `ELSE` is used for an alternative, e.g. `<IF...>A</IF><IF ELSE>B</IF>` would produce `A` or `B` depending on the initial `IF` condition.

## WHILE

The `<WHILE...>` tag works the same way as if, except that if the condition is true, after processing the contents up to the corresponding `</WHILE>` the condition is evaluated again. Only when it is false does processing continue after the `</WHILE>`. This is typically used with `EVAL` to create a loop, though for simple loops `FOR` can be used.

## FOR

The `<FOR...>` tag has one or more attributes name=value, and iterates the contents of the `<FOR>...</FOR>` setting the named environment variable each time. The variable is set to each word in the value. The words are spaces by `TAB` usually, but if the attribute `TAB`, `HASH`, `SPACE`, `NL`, or `LF` are included before name=value then they change the delimiter. E.g. `<FOR SPACE A="1 2 3">[<output name=A>]</FOR>` will output `[1][2][3]`.

There is also a special case for use of `<FOR>...</FOR>` for simple loops. Where the value has two words only that are integers, and where `UP` or `DOWN` is included before it, that causes the value to be treated as an integer and go or down one at a time from the first to last word.

It is also possible to loop time/dates using `DAY`, `MONTH`, `YEAR`, `WEEK`, `HOUR`, `MINUTE`, or `SECOND` as a tag before the `name=value`. This expects two words in the value that are ISO date/time format and will iterate up from one to the other in the interval specified.

## DIR

Directory listing. With no `PATH` set this is directory listing of current directory. If `PATH=` is a name of a directory, then that is listed, otherwise `PATH` is expanded as normal shell glob to a list of files and they are processed.

For each file, the variables are set for `FILENAME`, `FILELEAF`, `FILEEXT`, `FILESIZE`, `FILETYPE`, `FILEMODE`, `FILEMTIME`, `FILECTIME`, `FILEATIME`, and the enclosed XML processed. Normally a directory list ignores files starting with a dot, but including ALL includes these.

## SCRIPT

The `<SCRIPT...>` tag can include `var=name` one or more times which causes `var name='value';` to be added to the start of the script content. The special case `var=*` causes all columns in the current `<SQL...>` query to be output.

You can include `object=name` which creates a single new `var` called name with all of the listed vars within it rather than as separate `var` declarations. This is recommended as it avoids creating unnecessary global variables in javascript.

The name of the variable can also be prefixed with a `*`, e.g. `var=*name` to indicate the value is to be output raw unquoted, this is mainly where an SQL query set the value of the variable using JSON or JSARRAY. Bear in mind if the values is not valid JSON you could execute any script, etc, from the variable (or file if used with `@`) in javascript.

The name of the variable can also be prefixed with an `@`, e.g. `var=@name` to indicate the value is the content of a file which is in name. You can use `@` and `*`. This is risky as the variable could be any file name on the system.

## IMG

An `<IMG...>` tag with attribute `base64=filename` will check the file exists and confirm if it looks like a PNG, GIF, or JPEG file. If so, it will create a `src=` with base64 data URI encoded content of the file. Any existing `src=` is stripped. If the file cannot be recognised then the `<IMG...>` is output anyway, thus allowing `src=` to be used as a fallback.

## MARKUP format

Markup output format is intended to be used where what is being output has been input by end users. Markup that is output as markup is restricted to certain specific case that are safe. e.g. it has to be balanced so you could not, for example, start a bold tag and not end it so leaving the rest of the page bold. Only the following markup is allowed, and all other cases are escaped.

- Balanced `<b>` for bold text.
- Balanced `<i>` for italic text.
- Balanced `<s>` for struck out text.
- Balanced `<u>` for underlined text.
- Balanced `<blink>` for blinking text.
- Use of `www.` followed by a valid format domain made in to a link to that domain.
- Use of `http://domain/link` or `https://domain/link` make in to a link to that domain.
- Use of any smilies where `smiley=` is used on `xmlsql` command.
- Other balanced markup with no attributes, e.g. `<quote>xxx</quote>` are coded as a `span`, e.g. `<span class="quote">xxx</span>`
- To provide smilies you need a directory containing `.gif`, `.png`, `.jpg` (or `.jpeg`), or `.svg` files. The text before the extension is the smiley (at least two characters). e.g. a file called `:-).gif` is a smiley image for `:-)`. The smiley directory is specified as a relative link which must also be valid in the URL, e.g. `--smiley=smilies` with `:-).gif` in it would change any `:-)` to `<img src='smilies//%3A%2D%29.gif' />`

Note that there is expected to be white space before and after `www.domain` or `http://...` or `https://...`, as well as before any smiley. A smiley must not be followed by any alphanumeric character.

The `<b>`, etc, tags must be balance properly and must not have any other attributes. e.g. `<b style='color:red;'>` is not valid in this context.

## FORM

There is an option to remove forms `--no-form`.

There is an option to include a hidden field in forms `--security`. This defaults to the value of environment variable `*`. This is used with security functions in commands `envpass` and `password`.
