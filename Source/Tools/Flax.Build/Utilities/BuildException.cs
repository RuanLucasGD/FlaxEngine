// Copyright (c) 2012-2023 Wojciech Figat. All rights reserved.

using System;

namespace Flax.Build
{
    internal class BuildException : Exception
    {
        public BuildException(string message)
        : base(message)
        {
        }
    }
}
